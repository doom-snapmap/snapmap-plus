#include <limits.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_runtime.h"
#include "package_usage.h"
#include "resource_catalog.h"
#include "backend_log.h"
#include "overrides_baked.h"
#include "decl_native_schema.h"
#include "decl_script_native.h"
#include "resource_types.h"
#include "decl_graph_compose.h"
#include "decl_md6_compose.h"
#include "decl_block_compose.h"
#include "package_keys.h"
#include "package_material.h"
#include "grid_room_asset.h"
#include "resource_graph.h"
#include "audio_originals.h"
#include "audio_files_native.h"
#include "package_library.h"
#ifndef SH_PACKAGE_RUNTIME_TESTING
#include "spawn_keys.h"
#endif
#include "package_hud.h"
#include "package_sound.h"
#include "package_native_assets.h"
#include "package_map_uses.h"
#include "map_embed.h"
#include "map_provenance.h"
#ifndef SH_PACKAGE_RUNTIME_TESTING
#include "decl_server.h"
#include "resource_graph_native.h"
#include "engine_globals.h"
#include "host_image.h"
#include "strids.h"
#include "package_sound_native.h"
#include "audio_banks_native.h"
#endif

static SRWLOCK g_lock = SRWLOCK_INIT;
static SRWLOCK g_refresh_lock = SRWLOCK_INIT;
/* Ownership changes are serialized by g_refresh_lock. Published pointers and
 * aliases change together under g_lock. A view borrows its catalog through a
 * retained library parent; every compilation/inventory remains independently
 * owned. Readers hold g_lock, not an untracked provider pointer. */
typedef struct pr_provider {
    sh_package_sources *sources;
    /* Original metadata for retained serving-only owners. Never author paths
     * to reread: resources and complete bundles already own immutable bytes. */
    sh_package_sources *authored_sources;
    sh_package_bundle **bundles; /* Indexed by this inventory's exact owner. */
    size_t bundle_count;
    sh_package_compilation *compiled;
    sh_resource_catalog *catalog;
    sh_audio_originals *audio_originals;
    sh_package_native_assets *native_assets;
    sh_virtual_material_view *virtual_materials;
    sh_package_consumers *consumers;
    sh_package_audio_program *audio;
    /* Views retain every root provider whose audio program they serve: the
     * current map, the library and retained occurrence/selector owners. The
     * native audio layer retains these same owners until consumers retire. */
    struct pr_provider **audio_sources;
    size_t audio_source_count;
#ifndef SH_PACKAGE_RUNTIME_TESTING
    sh_strids_reservation *strings;
    sh_audio_id_lease *audio_ids;
#endif
    sh_package_provenance bindings;
    sh_package_provenance policy_uses; /* explicit active document, not resource supplier union */
    sh_package_provenance historical_bindings; /* Original names before private historical linking. */
    sh_package_references required_paths;
    sh_package_file_identity required_map;
    volatile LONG requirements_known;
    struct pr_provider *parent;
    char *data_root;
    char *rejections;
    char *unserved; /* Valid installed packages this build cannot link yet. */
    char *coverage; /* Accounted missing product coverage; packages still served. */
    sh_package_key_groups *key_groups; /* Group codes its relocated values use. */
    volatile LONG references;
} pr_provider;
typedef struct pr_occurrence_lease {
    volatile LONG references;
    pr_provider *provider;
    sh_package_provenance context; /* one exact authored context */
    char *family, *original, *name; /* exact selected native unit, not its siblings */
    struct pr_occurrence_lease *next;
} pr_occurrence_lease;
static SRWLOCK g_lease_lock = SRWLOCK_INIT;
static pr_occurrence_lease *g_leases;
struct sh_package_selector_hold {
    volatile LONG live;
    pr_provider *provider;
    struct sh_package_selector_hold *next;
};
static sh_package_selector_hold *g_selectors;
static volatile LONG g_lease_generation, g_lease_published;
static sh_package_provenance g_promoted;
static LONG g_promotion_generation; /* protected by g_lock */
typedef struct pr_inventory_entry { char *path; size_t source; } pr_inventory_entry;
typedef struct pr_inventory {
    sh_package_sources *sources;
    pr_inventory_entry *entries;
    size_t count, references;
} pr_inventory;
typedef struct pr_availability { const pr_provider *library; const pr_inventory *inventory; } pr_availability;
static int pr_inventory_available(void *context, const char *path, char *error, size_t capacity);
struct sh_package_map_plan {
    pr_provider *provider;
    pr_inventory *inventory;
    pr_provider *library, *library_base, *authoring;
    int occurrences_admitted, inventory_committed;
};
static int pr_plan_library_current(const sh_package_map_plan *plan, char *error, size_t capacity);
static int pr_plan_authoring_served(const sh_package_map_plan *plan,
    const sh_package_compilation *serving, char *error, size_t capacity);
static pr_provider *g_current_provider, *g_library_provider, *g_map_provider;
/* Availability is a source-library fact, independent of which compatible
 * package set currently supplies the editor or the active map. */
static pr_inventory *g_inventory;
static sh_package_sources *g_sources;
static sh_package_compilation *g_compiled;
static sh_resource_catalog *g_catalog;
static int g_ready;
static int g_usable;
static int g_activating;
static int g_native_preparing;
static pr_provider *g_occurrence_recovery_provider;
static void *g_occurrence_recovery_context;
static int g_occurrence_recovery_ready, g_occurrence_recovery_usable;
/* Native activation runs with this thread owning the refresh lock. Editor
 * callbacks may inspect the published candidate without reacquiring it. */
static __declspec(thread) unsigned g_activation_callback;
static char g_error[2048];
static char g_authoring_error[2048];
static char *g_map_json;
static size_t g_map_length;
static sh_package_references g_map_references;
static sh_package_policy g_map_policy;
static uintptr_t g_native_accessor, g_native_types, g_native_game_system;
static sh_decl_script_schema *g_native_scripts;
const sh_decl_script_schema *sh_package_runtime_script_schema(void) { return g_native_scripts; }
static int g_native_bound;
/* Set when the package tree gained or lost sources behind the compiled library:
 * a whole-package installation commits its files while a temporary map provider
 * is already carrying them. Until the library is recompiled those resources exist
 * only in the overlay, so retiring the overlay would retire live content. */
static volatile LONG g_sources_changed;

void sh_package_runtime_note_sources_changed(void)
{
    InterlockedExchange(&g_sources_changed, 1);
}

static pr_provider *pr_retain(pr_provider *provider)
{
    if (provider) InterlockedIncrement(&provider->references);
    return provider;
}

static void pr_release(pr_provider *provider)
{
    if (!provider || InterlockedDecrement(&provider->references)) return;
#ifndef SH_PACKAGE_RUNTIME_TESTING
    sh_strids_reservation_free(provider->strings);
    sh_audio_id_lease_release(provider->audio_ids);
#endif
    if (!sh_virtual_material_runtime_retire(&provider->virtual_materials))
        backend_log("package provider released an active virtual material view");
    sh_package_compilation_free(provider->compiled);
    sh_package_audio_program_close(provider->audio);
    sh_package_consumers_free(provider->consumers);
    sh_package_provenance_free(&provider->bindings);
    sh_package_provenance_free(&provider->policy_uses);
    sh_package_provenance_free(&provider->historical_bindings);
    sh_package_references_free(&provider->required_paths);
    for (size_t i = 0; i < provider->bundle_count; i++) sh_package_bundle_release(provider->bundles[i]);
    free(provider->bundles);
    sh_package_sources_free(provider->sources);
    sh_package_sources_free(provider->authored_sources);
    if (!provider->parent) {
        sh_resource_catalog_close(provider->catalog);
        sh_audio_originals_close(provider->audio_originals);
        sh_package_native_assets_close(provider->native_assets);
    }
    for (size_t i = 0; i < provider->audio_source_count; i++) pr_release(provider->audio_sources[i]);
    free(provider->audio_sources);
    pr_release(provider->parent); free(provider->data_root); free(provider->rejections);
    free(provider->unserved); free(provider->coverage); free(provider->key_groups); free(provider);
}
/* Precedence follows call order; one owner appears once. A view contributes
 * the owners it already retains, a root provider itself when it has audio. */
static int pr_audio_source_add(pr_provider *view, const pr_provider *source)
{
    if (!source) return 1;
    if (!source->audio) {
        for (size_t i = 0; i < source->audio_source_count; i++)
            if (!pr_audio_source_add(view, source->audio_sources[i])) return 0;
        return 1;
    }
    for (size_t i = 0; i < view->audio_source_count; i++) if (view->audio_sources[i] == source) return 1;
    if (view->audio_source_count >= SIZE_MAX / sizeof(*view->audio_sources) - 1) return 0;
    pr_provider **grown = realloc(view->audio_sources, (view->audio_source_count + 1) * sizeof(*grown));
    if (!grown) return 0;
    view->audio_sources = grown;
    view->audio_sources[view->audio_source_count++] = pr_retain((pr_provider *)source);
    return 1;
}

/* The spawn-key detours read the group codes of the providers now serving:
 * the installed library's and the active map's. */
#ifdef SH_PACKAGE_RUNTIME_TESTING
static const void *g_test_key_groups[4];
#endif
static void pr_publish_key_groups(void)
{
#ifndef SH_PACKAGE_RUNTIME_TESTING
    if (!sh_spawn_keys_publish_groups(g_library_provider ? g_library_provider->key_groups : NULL,
        g_map_provider ? g_map_provider->key_groups : NULL))
        backend_log("SPAWN-KEYS: the map's group codes disagree with the library's; the previous codes stay");
#else
    g_test_key_groups[0] = g_library_provider; g_test_key_groups[1] = g_library_provider ? g_library_provider->key_groups : NULL;
    g_test_key_groups[2] = g_map_provider; g_test_key_groups[3] = g_map_provider ? g_map_provider->key_groups : NULL;
#endif
}
static void pr_publish(pr_provider *current, pr_provider *library, pr_provider *map)
{
    g_current_provider = current; g_library_provider = library; g_map_provider = map;
    pr_publish_key_groups();
    g_sources = current ? current->sources : NULL;
    g_compiled = current ? current->compiled : NULL;
    g_catalog = current ? current->catalog : NULL;
}

int sh_package_runtime_consumer_acquire(const char *family, const char *name, sh_package_consumer_hold *out)
{
    if (!out || out->provider || out->binding || !family || !*family || !name || !*name) return -1;
    AcquireSRWLockShared(&g_lock);
    const sh_package_consumer *binding = sh_package_consumers_find(
        g_current_provider ? g_current_provider->consumers : NULL, family, name);
    if (binding) { out->provider = pr_retain(g_current_provider); out->binding = binding; }
    ReleaseSRWLockShared(&g_lock); return binding != NULL;
}
void sh_package_runtime_consumer_release(sh_package_consumer_hold *hold)
{
    if (!hold) return;
    pr_provider *provider = hold->provider;
    memset(hold, 0, sizeof(*hold)); pr_release(provider);
}

static int pr_native_read(void *context, uintptr_t address, void *out, size_t length)
{
    (void)context;
    if (!address || length > UINTPTR_MAX - address) return 0;
    __try { memcpy(out, (const void *)address, length); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void sh_package_runtime_bind_native(uintptr_t accessor, uintptr_t type_container,
    uintptr_t game_system_export)
{
    AcquireSRWLockExclusive(&g_refresh_lock);
    g_native_accessor = accessor; g_native_types = type_container;
    g_native_game_system = game_system_export; g_native_bound = 1;
    ReleaseSRWLockExclusive(&g_refresh_lock);
}

int sh_package_runtime_native_status(void)
{
    sh_decl_native_source source = {NULL, pr_native_read, 0, g_native_scripts};
    int state;
    AcquireSRWLockShared(&g_refresh_lock);
    state = sh_decl_native_source_ready(&source, g_native_accessor, g_native_types, g_native_game_system);
    ReleaseSRWLockShared(&g_refresh_lock); return state;
}

static int pr_class_derives(void *context, const char *child, const char *parent)
{
    return sh_decl_native_schema_class_derives((sh_decl_native_schema *)context, child, parent);
}

static int pr_declaration_state(void *context, const char *type, sh_decl_value_type *state)
{
    return sh_decl_native_schema_decl_type((sh_decl_native_schema *)context, type, state);
}

static char *pr_canonical(const char *path)
{
    char *normalized = sh_package_engine_path(path), *generated = NULL;
    int claimed;
    if (!normalized) return NULL;
    claimed = sh_grid_asset_canonical(normalized, &generated);
    if (!claimed) return normalized;
    free(normalized); return claimed > 0 ? generated : NULL;
}

static int pr_compose_custom(void *context, const char *type, sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, char **body, size_t *length,
    char *error, size_t capacity, sh_decl_conflict *conflict, const sh_package_source_view *source_view)
{
    sh_decl_native_schema *native = context;
    sh_decl_value_type root = {0};
    int result;
    *body = NULL; *length = 0;
    if (!strcmp(type, "material")) {
        *body = sh_package_material_compose(baseline, sources, count, source_view,
            length, error, capacity, conflict);
        return *body ? 1 : -1;
    }
    if (!strcmp(type, "md6def")) {
        *body = sh_decl_md6_compose(baseline, sources, count, length, error, capacity, conflict);
        return *body ? 1 : -1;
    }
    if (sh_decl_block_family(type)) {
        *body = sh_decl_block_compose(type, baseline, sources, count, length, error, capacity, conflict);
        return *body ? 1 : -1;
    }
    result = sh_decl_native_schema_decl_type(native, type, &root);
    if (result > 0) result = sh_decl_native_schema_graph_type(native, root);
    if (result < 0) { snprintf(error, capacity, "native graph reader metadata is unavailable"); return -1; }
    if (!result) return 0;
    *body = sh_decl_graph_compose(baseline, sources, count, sh_decl_native_schema_view(native),
        root, length, error, capacity, conflict);
    return *body ? 1 : -1;
}

int sh_package_runtime_declaration_families(sh_package_family_visit visit, void *context,
    char *error, size_t capacity)
{
    sh_decl_native_source source = {NULL, pr_native_read, 0, g_native_scripts};
    sh_decl_native_schema *schema;
    size_t reported = 0;
    if (error && capacity) error[0] = 0;
    if (!visit) return -1;
    AcquireSRWLockShared(&g_refresh_lock);
    if (sh_decl_native_source_ready(&source, g_native_accessor, g_native_types,
            g_native_game_system) != 1) {
        ReleaseSRWLockShared(&g_refresh_lock);
        if (error && capacity) snprintf(error, capacity, "native declaration metadata is unavailable");
        return -1;
    }
    schema = sh_decl_native_schema_open(source, (sh_decl_dependency_observer){0}, error, capacity);
    ReleaseSRWLockShared(&g_refresh_lock);
    if (!schema) return -1;
    for (size_t i = 0; i < sizeof(SH_RESOURCE_TYPES) / sizeof(SH_RESOURCE_TYPES[0]); i++) {
        const char *type = SH_RESOURCE_TYPES[i].type;
        sh_decl_value_type state = {0};
        const char *route, *name = "";
        int resolved, graph = 0;
        size_t previous;
        if (!type || !*type) continue;
        for (previous = 0; previous < i; previous++)
            if (!strcmp(SH_RESOURCE_TYPES[previous].type, type)) break;
        if (previous < i) continue;
        resolved = sh_decl_native_schema_decl_type(schema, type, &state);
        if (resolved > 0) {
            name = state.name ? state.name : "";
            graph = sh_decl_native_schema_graph_type(schema, state);
        }
        if (!strcmp(type, "material")) route = "material adapter";
        else if (!strcmp(type, "md6def")) route = "md6 adapter";
        else if (sh_decl_block_family(type)) route = "block adapter";
        else if (resolved < 0 || graph < 0) route = "metadata unavailable";
        else if (graph > 0) route = "graph adapter";
        else if (resolved > 0) route = "reflected state";
        else route = "custom reader, no adapter";
        visit(context, type, name, route);
        reported++;
    }
    sh_decl_native_schema_close(schema);
    return (int)reported;
}

static pr_inventory *pr_inventory_retain(pr_inventory *inventory)
{ if (inventory) inventory->references++; return inventory; }

static void pr_inventory_release(pr_inventory *inventory)
{
    if (!inventory || --inventory->references) return;
    for (size_t i = 0; i < inventory->count; i++) free(inventory->entries[i].path);
    free(inventory->entries); sh_package_sources_free(inventory->sources); free(inventory);
}

static int pr_inventory_compare(const void *a, const void *b)
{
    const pr_inventory_entry *left = a, *right = b;
    int order = strcmp(left->path, right->path);
    return order ? order : left->source < right->source ? -1 : left->source > right->source;
}

/* Takes source ownership even on failure. Build once; admission then searches
 * the index instead of rescanning/canonicalizing every installed file per path. */
static pr_inventory *pr_inventory_build(sh_package_sources *sources, char *error, size_t capacity)
{
    pr_inventory *inventory = calloc(1, sizeof(*inventory));
    if (!inventory) { sh_package_sources_free(sources); goto failed; }
    inventory->references = 1; inventory->sources = sources;
    if (sources->file_count > SIZE_MAX / sizeof(*inventory->entries)) goto bad;
    inventory->entries = calloc(sources->file_count ? sources->file_count : 1, sizeof(*inventory->entries));
    if (!inventory->entries) goto bad;
    for (size_t i = 0; i < sources->file_count; i++) {
        const sh_package_source_file *file = &sources->files[i];
        pr_inventory_entry *entry;
        if (!file->engine_path || file->directory) continue;
        entry = &inventory->entries[inventory->count];
        entry->path = pr_canonical(file->engine_path); entry->source = i;
        if (!entry->path) goto bad;
        inventory->count++;
    }
    qsort(inventory->entries, inventory->count, sizeof(*inventory->entries), pr_inventory_compare);
    return inventory;
bad:
    pr_inventory_release(inventory);
failed:
    if (error && capacity) snprintf(error, capacity, "cannot index the complete installed resource inventory");
    return NULL;
}

typedef struct pr_producer_view {
    sh_package_producer_reader read;
    void *context;
} pr_producer_view;

static unsigned char *pr_producer_read(void *context, const char *path, size_t *length)
{
    pr_producer_view *view = (pr_producer_view *)context;
    unsigned char *body = NULL;
    int result = view->read(view->context, path, &body, length);
    if (result > 0) return body;
    free(body); *length = 0; return NULL;
}

static void pr_producer_release(void *context, void *body)
{ (void)context; free(body); }

static int pr_produce(void *context, const char *path, sh_package_producer_reader read,
    void *read_context, unsigned char **body, size_t *length)
{
    pr_producer_view view = {read, read_context};
    (void)context;
    return sh_grid_asset_open(path, pr_producer_read, pr_producer_release, &view, body, length);
}

typedef struct pr_original_source {
    sh_package_baseline_reader game;
    void *context;
    sh_package_native_assets *native_assets;
} pr_original_source;

static int pr_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    pr_original_source *source = (pr_original_source *)context;
    size_t i;
    int scope;
    *body = NULL; *length = 0;
    scope = sh_package_native_assets_shader_read(source->native_assets, path, body, length);
    if (scope) return scope; /* One evidenced native program/permutation generation. */
    scope = source->game ? source->game(source->context, path, body, length) : 0;
    if (scope) return scope; /* An unreadable game original remains an error. */
    free(*body); *body = NULL; *length = 0;
    scope = sh_package_native_assets_read(source->native_assets, path, body, length);
    if (scope) return scope;
    for (i = 0; i < sizeof(g_ov_baked_decls) / sizeof(g_ov_baked_decls[0]); i++) {
        const ov_baked_decl_t *value = &g_ov_baked_decls[i];
        if (_stricmp(path, value->name)) continue;
        *body = (unsigned char *)malloc((size_t)value->len + 1);
        if (!*body) return -1;
        memcpy(*body, value->text, value->len); (*body)[value->len] = 0;
        *length = value->len;
        return 3;
    }
    return 0;
}

static int pr_cache_directory(const char *root, char *out, size_t capacity)
{
    wchar_t *wide = sh_package_source_wide_path(root);
    size_t i;
    int ok = 1;
    if (!wide) return 0;
    /* Refuse redirected ancestors, including the configured data directory. */
    for (i = 7; ; i++) if (!wide[i] || wide[i] == L'\\') {
        wchar_t saved = wide[i];
        DWORD attributes;
        wide[i] = 0; attributes = GetFileAttributesW(wide); wide[i] = saved;
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { ok = 0; break; }
        if (!saved) break;
    }
    free(wide);
    if (!ok) return 0;
    for (i = 0; i < 2; i++) {
        DWORD attributes;
        if (snprintf(out, capacity, "%s/package-cache%s", root, i ? "/resources" : "") >= (int)capacity) return 0;
        wide = sh_package_source_wide_path(out);
        if (!wide) return 0;
        if (!CreateDirectoryW(wide, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ok = 0;
        attributes = GetFileAttributesW(wide); free(wide);
        if (!ok || attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    }
    return 1;
}

static int pr_cache_resources(sh_package_compilation *compiled, const char *root,
                               char *error, size_t capacity)
{
    char directory[4096] = "";
    for (size_t i = 0; i < compiled->resource_count; i++) {
        sh_compiled_resource *resource = &compiled->resources[i];
        if (resource->body || resource->cache_file) continue;
        if (!compiled->sources || resource->source >= compiled->sources->file_count) {
            snprintf(error, capacity, "resource has no retained cache source: %s", resource->engine_path); return 0;
        }
        if (!directory[0] && !pr_cache_directory(root, directory, sizeof(directory))) {
            snprintf(error, capacity, "cannot prepare package resource cache"); return 0;
        }
        resource->cache_file = sh_package_source_cache(compiled->sources->files + resource->source,
            directory, &resource->cache_path, error, capacity);
        if (!resource->cache_file) return 0;
    }
    return 1;
}

#ifdef SH_PACKAGE_RUNTIME_TESTING
/* Explicit empty-original catalog for filesystem/compiler integration tests.
 * Production always reads the installed game's catalog. */
static int g_test_empty_catalog;
static sh_package_baseline_reader g_test_baseline;
static void *g_test_baseline_context;
static sh_decl_registry_source g_test_source_registry;
static uintptr_t g_test_source_reflection;
static int g_test_installed_links;
void sh_package_runtime_test_installed_links(int enabled) { g_test_installed_links = enabled; }
static const char *g_test_audio_root;
static const char *g_test_audio_bank_prefix;
static const wchar_t *g_test_audio_language;
void sh_package_runtime_test_audio_originals(const char *root, const wchar_t *language)
{ g_test_audio_root = root; g_test_audio_language = language; }
void sh_package_runtime_test_empty_catalog(void) { g_test_empty_catalog = 1; }
void sh_package_runtime_test_baseline(sh_package_baseline_reader reader, void *context)
{
    g_test_empty_catalog = 1; g_test_baseline = reader; g_test_baseline_context = context;
}
void sh_package_runtime_test_source_readers(sh_decl_registry_source registry, uintptr_t reflection)
{ g_test_source_registry = registry; g_test_source_reflection = reflection; }
static sh_package_source_implicit g_test_constructor;
static void *g_test_constructor_context;
void sh_package_runtime_test_constructor_requests(sh_package_source_implicit capture, void *context)
{ g_test_constructor = capture; g_test_constructor_context = context; }
static int pr_test_constructor(void *context, size_t scope, int observing, const sh_package_link_unit *unit,
    int (*visit)(void *, const char *, const char *, const char *), void *visitor, char *error, size_t capacity)
{
    (void)context;
    return !g_test_constructor || g_test_constructor(g_test_constructor_context,
        scope, observing, unit, visit, visitor, error, capacity) == 1;
}
int sh_package_runtime_test_source_inputs(sh_decl_registry_source *registry, uintptr_t *reflection,
    sh_package_baseline_reader *baseline, void **baseline_context)
{
    if (registry) *registry = g_test_source_registry;
    if (reflection) *reflection = g_test_source_reflection;
    if (baseline) *baseline = g_test_baseline;
    if (baseline_context) *baseline_context = g_test_baseline_context;
    return g_test_source_registry.read && g_test_source_registry.registry && g_test_source_reflection;
}
#endif

static int pr_activate(sh_package_activation_fn activate, void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    int result = 0;
    g_activation_callback++;
    __try {
        __try {
            result = activate(context, restoring, changes, error, capacity) == 1 &&
                sh_virtual_material_runtime_status(error, capacity);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            snprintf(error, capacity, "native package %s raised an exception (0x%08lx)",
                restoring ? "recovery" : "activation", (unsigned long)GetExceptionCode());
        }
    }
    __finally { g_activation_callback--; }
    return result;
}

static int pr_rejected(void *context, const sh_package *package, const char *reason)
{
    char **report = context;
    size_t used = *report ? strlen(*report) : 0;
    size_t add = strlen(package->root) + strlen(reason) + 96;
    char *grown;
    if (used > SIZE_MAX - add || !(grown = realloc(*report, used + add))) return 0;
    *report = grown;
    snprintf(grown + used, add, "Skipped local package %s: %s\n", package->root, reason);
    backend_log(grown + used);
    return 1;
}
/* Missing product coverage is not an author error: the delivery remains
 * installed and available; only the current library candidate omits it. */
static int pr_unserved(char **report, const sh_package *package, const char *reason)
{
    size_t used = *report ? strlen(*report) : 0;
    size_t add = strlen(package->root) + strlen(reason) + 128;
    char *grown;
    if (used > SIZE_MAX - add || !(grown = realloc(*report, used + add))) return 0;
    *report = grown;
    snprintf(grown + used, add, "Local package %s is not served yet (product coverage): %s\n",
        package->root, reason);
    backend_log(grown + used);
    return 1;
}

/* A path without its own native identity, such as a shared shader include,
 * names every program catalogued beneath it. Campaign-only programs are not
 * SnapMap resources: the source link never captures them through a shared
 * include, so a historical context must not require them either. A map that
 * needs one supplies its declaration, which carries its own identity. */
static int pr_campaign_program(const sh_resource_catalog_entry *row)
{ return row->archive >= 2 && !strcmp(row->type, "renderprog"); }

/* Derive old-name bindings from the historical composition before private
 * linking. Every known root retains the complete original contributor context;
 * this recipe is separate from the newly linked native binding catalog. */
static int pr_legacy_bindings(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_provenance *out)
{
    const sh_package_sources *sources = compiled->sources;
    size_t context = SIZE_MAX;
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = compiled->resources + i;
        const sh_resource_catalog_entry *const *entries = NULL;
        size_t native_count = sh_package_resource_native_count(resource);
        sh_package_native_identity native = sh_package_resource_native_at(resource, 0);
        const char *family = native.type, *name = native.name;
        char *cooked = NULL;
        size_t count = 0;
        int ok = 1;
        if (!resource->source_count) continue;
        if (!family || !name) {
            count = sh_resource_catalog_find_provider(catalog, resource->engine_path, &entries);
            if (!count) {
                int status = sh_package_cooked_identity(resource->engine_path, &family, &cooked);
                if (status < 0) return 0;
                if (!status) continue;
                name = cooked;
            }
        }
        if (context == SIZE_MAX) context = sh_package_provenance_context_add(out,
            SH_PACKAGE_CONTEXT_COMPOSED, sources->original_fingerprints ?
                sources->original_fingerprints : sources->fingerprints, sources->package_count, NULL);
        if (context == SIZE_MAX) { free(cooked); return 0; }
        if (count) {
            for (size_t e = 0; e < count && ok; e++)
                if (*entries[e]->type && *entries[e]->name && !pr_campaign_program(entries[e]))
                    ok = sh_package_provenance_bind(out, entries[e]->type, entries[e]->name,
                        entries[e]->name, &context, 1);
        } else if (native_count) {
            for (size_t n = 0; n < native_count && ok; n++) {
                native = sh_package_resource_native_at(resource, n);
                ok = sh_package_provenance_bind(out, native.type, native.name, native.name, &context, 1);
            }
        } else ok = sh_package_provenance_bind(out, family, name, name, &context, 1);
        free(cooked); if (!ok) return 0;
    }
    return 1;
}
#ifdef SH_PACKAGE_RUNTIME_TESTING
int sh_package_runtime_test_legacy_bindings(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_provenance *out)
{ return pr_legacy_bindings(compiled, catalog, out); }
#endif

/* SnapMap itself serves this name from this exact file. */
static int pr_snapmap_serves(void *context, const char *path, const char *type, const char *name)
{
    const sh_resource_catalog_entry *const *rows = NULL;
    size_t count = sh_resource_catalog_find((const sh_resource_catalog *)context, type, name, &rows);
    for (size_t i = 0; i < count; i++) if (rows[i]->archive < 2 && !_stricmp(rows[i]->path, path)) return 1;
    return 0;
}

static int pr_capture_bundles(pr_provider *provider, const sh_package_sources *sources,
    const char *data_root, char *error, size_t capacity)
{
    if (sources->package_count > SIZE_MAX / sizeof(*provider->bundles)) return 0;
    provider->bundles = calloc(sources->package_count ? sources->package_count : 1u,
        sizeof(*provider->bundles));
    if (!provider->bundles) return 0;
    provider->bundle_count = sources->package_count;
    for (size_t i = 0; i < sources->package_count; i++) {
        provider->bundles[i] = sh_package_bundle_capture(sources, i, data_root, error, capacity);
        if (!provider->bundles[i]) return 0;
    }
    return 1;
}

/* Map extraction has already verified the relation between each original
 * delivery and its normalized compiler tree. Match exact inventoried owners;
 * descriptor IDs and a matching tree alone cannot select the delivery. */
static sh_package_bundle **pr_match_bundles(const sh_package_sources *sources,
    const sh_package_bundle_binding *bindings, size_t count, char *error, size_t capacity)
{
    sh_package_bundle **next = NULL;
    if (!sources || count != sources->package_count || (count && !bindings) ||
        count > SIZE_MAX / sizeof(*next)) goto failed;
    next = calloc(count ? count : 1u, sizeof(*next));
    if (!next) goto failed;
    for (size_t b = 0; b < count; b++) {
        size_t owner;
        if (!bindings[b].original || !memchr(bindings[b].source_name, 0, sizeof(bindings[b].source_name))) goto failed;
        for (owner = 0; owner < count; owner++)
            if (!strcmp(bindings[b].source_name, sources->packages[owner].name) &&
                !memcmp(bindings[b].compiler_tree, sources->fingerprints[owner], 32)) break;
        if (owner == count || next[owner]) goto failed;
        next[owner] = sh_package_bundle_retain(bindings[b].original);
    }
    return next;
failed:
    if (next) {
        for (size_t i = 0; i < count; i++) sh_package_bundle_release(next[i]);
        free(next);
    }
    if (error && capacity) snprintf(error, capacity,
        "complete original bundles must match every exact normalized source owner");
    return NULL;
}

static int pr_view_bundles(pr_provider *view, const pr_provider *library, const pr_provider *map,
    char *error, size_t capacity)
{
    size_t local_count = library->sources->package_count;
    size_t map_count = map ? map->sources->package_count : 0;
    if (local_count > SIZE_MAX - map_count || view->sources->package_count != local_count + map_count ||
        view->sources->package_count > SIZE_MAX / sizeof(*view->bundles)) goto failed;
    view->bundles = calloc(view->sources->package_count ? view->sources->package_count : 1u,
        sizeof(*view->bundles));
    if (!view->bundles) goto failed;
    view->bundle_count = view->sources->package_count;
    for (size_t i = 0; i < view->bundle_count; i++) {
        const pr_provider *source = i < local_count ? library : map;
        size_t owner = i < local_count ? i : i - local_count;
        if (!source || owner >= source->bundle_count || !source->bundles[owner] ||
            !source->sources->fingerprints || !view->sources->fingerprints ||
            memcmp(source->sources->fingerprints[owner], view->sources->fingerprints[i], 32)) goto failed;
        view->bundles[i] = sh_package_bundle_retain(source->bundles[owner]);
    }
    return 1;
failed:
    snprintf(error, capacity, "cannot retain complete source bundles for provider view"); return 0;
}

#if !defined(SH_PACKAGE_RUNTIME_TESTING) || defined(SH_PACKAGE_RUNTIME_NATIVE_TESTING)
/* Resume the same immutable source inventory after explicit native source
 * construction. No compiler callback may construct an engine resource. */
static int pr_prepare_native_sources(sh_package_native_assets *assets,
    uint64_t **completed, size_t *completed_count, char *error, size_t capacity)
{
    sh_package_shader_preparation *batch = sh_package_native_assets_shader_take(assets);
    pr_provider *current = NULL, *library = NULL, *map = NULL;
    size_t count = sh_package_shader_preparation_count(batch);
    uint64_t *grown;
    LONG promotion_generation = 0;
    int result = 0, recovered = 1;
    if (!batch || !count ||
#ifdef SH_PACKAGE_RUNTIME_NATIVE_TESTING
        !sh_package_runtime_test_native_ready()
#else
        !sh_decl_server_shader_preparation_ready()
#endif
    ) goto done;
    for (size_t i = 0; i < count; i++) {
        const sh_package_shader_request *request = sh_package_shader_preparation_request(batch, i);
        if (!request) goto done;
        for (size_t j = 0; j < *completed_count; j++) if ((*completed)[j] == request->task_id) {
            snprintf(error, capacity, "native shader source evidence changed during the same compilation: %s", request->name);
            goto done;
        }
    }
    if (count > SIZE_MAX - *completed_count || count + *completed_count > SIZE_MAX / sizeof(*grown) ||
        !(grown = realloc(*completed, (count + *completed_count) * sizeof(*grown)))) goto done;
    *completed = grown;
    AcquireSRWLockExclusive(&g_lock);
    if (g_native_preparing || g_activating) { ReleaseSRWLockExclusive(&g_lock); goto done; }
    g_native_preparing = 1;
    current = pr_retain(g_current_provider); library = pr_retain(g_library_provider); map = pr_retain(g_map_provider);
    promotion_generation = g_promotion_generation;
    ReleaseSRWLockExclusive(&g_lock);
    ReleaseSRWLockExclusive(&g_refresh_lock);
    __try {
        int prepared =
#ifdef SH_PACKAGE_RUNTIME_NATIVE_TESTING
            sh_package_runtime_test_native_run(batch, error, capacity);
#else
            sh_decl_server_prepare_shaders(batch, error, capacity);
#endif
        result = prepared == 1; recovered = prepared >= 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(error, capacity, "native shader source preparation raised exception %08lx", GetExceptionCode());
        recovered = 0;
    }
    AcquireSRWLockExclusive(&g_refresh_lock);
    AcquireSRWLockExclusive(&g_lock);
    g_native_preparing = 0;
    if (!recovered) g_usable = 0;
    if (current != g_current_provider || library != g_library_provider || map != g_map_provider ||
        promotion_generation != g_promotion_generation) {
        snprintf(error, capacity, "package context changed during native source preparation"); result = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (result) for (size_t i = 0; i < count; i++)
        (*completed)[(*completed_count)++] = sh_package_shader_preparation_request(batch, i)->task_id;
done:
    pr_release(current); pr_release(library); pr_release(map);
    if (!result) sh_package_shader_preparation_revoke(batch);
    sh_package_shader_preparation_free(batch);
    if (!result && error && capacity && !*error)
        snprintf(error, capacity, "native shader source preparation is unavailable at this boundary");
    return result;
}
#ifdef SH_PACKAGE_RUNTIME_NATIVE_TESTING
int sh_package_runtime_test_prepare_native_sources(sh_package_native_assets *assets,
    uint64_t **completed, size_t *completed_count, char *error, size_t capacity)
{
    int result;
    if (error && capacity) *error = 0;
    AcquireSRWLockExclusive(&g_refresh_lock);
    result = pr_prepare_native_sources(assets, completed, completed_count, error, capacity);
    ReleaseSRWLockExclusive(&g_refresh_lock);
    return result;
}
#endif
#endif

static pr_provider *pr_prepare_deliveries(const char *data_root, const char *source_root,
    const pr_provider *previous, int product_only, int authoring_increment, pr_inventory **inventory_out,
    int *composition_failed, const sh_package_provenance *contexts,
    const sh_package_bundle_binding *deliveries, size_t delivery_count, int use_deliveries,
    const pr_provider *string_peer, char *out_error, size_t out_capacity)
{
    sh_package_sources *sources = NULL, *inventory_sources = NULL;
    sh_package_bundle **original_bundles = NULL;
    sh_package_compilation *compiled = NULL;
    sh_resource_catalog *catalog = NULL;
    sh_audio_originals *audio_originals = NULL;
    sh_package_native_assets *native_assets = NULL;
    sh_package_baseline_reader baseline = NULL;
    void *baseline_context = NULL;
    sh_package_builtin builtins[sizeof(g_ov_baked_decls) / sizeof(g_ov_baked_decls[0])];
    sh_package_compile_environment environment = {0};
    sh_decl_native_schema *native_schema = NULL;
    char doom_base[4096], error[2048] = "";
#ifndef SH_PACKAGE_RUNTIME_TESTING
    char doom_root[4096];
    sh_strids_reservation *strings = NULL;
    sh_audio_id_lease *audio_ids = NULL;
    uint64_t *prepared_tasks = NULL;
    size_t prepared_task_count = 0;
#else
    (void)string_peer;
#endif
    DWORD n;
    size_t default_index;
    char *slash, *rejections = NULL, *unserved = NULL;
    size_t invalid_package = SIZE_MAX, unserved_package = SIZE_MAX;
    sh_package_owners conflicted = {0};
    sh_package_library isolated = {0};
    int library_output = 0;
    sh_package_provenance historical = {0}, authoring_contexts = {0};
    uintptr_t reflection = 0;
    pr_provider *provider = NULL;
    if (inventory_out) *inventory_out = NULL;
    if (composition_failed) *composition_failed = 0;
    if (g_native_bound
#ifndef SH_PACKAGE_RUNTIME_TESTING
        || 1
#endif
    ) {
        sh_decl_native_source native_source = {NULL, pr_native_read, 0};
        int state = sh_decl_native_source_ready(&native_source,
            g_native_accessor, g_native_types, g_native_game_system);
        if (state != 1) {
            snprintf(error, sizeof(error), state == 0 ? "native declaration readers are still initializing" :
                "native declaration metadata bindings are unavailable"); goto done;
        }
        reflection = native_source.reflection;
#ifndef SH_PACKAGE_RUNTIME_TESTING
        if (!g_native_scripts && sh_decl_server_shader_preparation_ready()) {
            char script_error[256];
            g_native_scripts = sh_decl_script_native_capture_default(script_error, sizeof(script_error));
            if (!g_native_scripts) backend_log(script_error);
        }
#endif
        native_source.scripts = g_native_scripts;
        native_schema = sh_decl_native_schema_open(native_source, (sh_decl_dependency_observer){0}, error, sizeof(error));
        if (!native_schema) goto done;
        environment.types = sh_decl_native_schema_view(native_schema);
        environment.type_context = native_schema; environment.class_derives = pr_class_derives;
        environment.declaration_state = pr_declaration_state;
        environment.compose_custom = pr_compose_custom;
    }
    sources = product_only ? calloc(1, sizeof(*sources)) : source_root ?
        sh_package_sources_scan(source_root, error, sizeof(error)) :
        sh_package_sources_scan_local(data_root, pr_rejected, &rejections, error, sizeof(error));
    if (!sources) goto done;
    if (use_deliveries) {
        if (!source_root || !(original_bundles = pr_match_bundles(sources, deliveries,
            delivery_count, error, sizeof(error)))) goto done;
        if (sources->package_count > SIZE_MAX / sizeof(*sources->original_fingerprints)) goto done;
        sources->original_fingerprints = calloc(sources->package_count ? sources->package_count : 1u,
            sizeof(*sources->original_fingerprints));
        if (!sources->original_fingerprints) goto done;
        for (size_t i = 0; i < sources->package_count; i++)
            memcpy(sources->original_fingerprints[i], sh_package_bundle_fingerprint(original_bundles[i]), 32);
    }
    /* Built-ins can inherit vanilla parents even on the first launch with no
     * authored packages. Their typed source views still require originals. */
    if ((sources->package_count || sizeof(builtins) / sizeof(builtins[0]) ||
         (previous && previous->compiled->resource_count))
#ifdef SH_PACKAGE_RUNTIME_TESTING
        && !g_test_empty_catalog
#endif
    ) {
        n = GetModuleFileNameA(NULL, doom_base, sizeof(doom_base));
        if (!n || n >= sizeof(doom_base) || !(slash = strrchr(doom_base, '\\'))) goto done;
        *slash = 0;
#ifndef SH_PACKAGE_RUNTIME_TESTING
        strcpy_s(doom_root, sizeof(doom_root), doom_base);
#endif
        if (strcat_s(doom_base, sizeof(doom_base), "\\base")) goto done;
        catalog = sh_resource_catalog_open(doom_base, error, sizeof(error));
        if (!catalog) goto done;
    }
    if (catalog) { baseline = sh_resource_catalog_read_path; baseline_context = catalog; }
#ifndef SH_PACKAGE_RUNTIME_TESTING
    if (catalog) {
        native_assets = previous && previous->native_assets ?
            sh_package_native_assets_retain(previous->native_assets) : sh_package_native_assets_open(doom_root);
        if (!native_assets) goto done;
        if (!previous || !previous->native_assets) {
            if (!sh_package_native_assets_configure_consumers(native_assets,
                sh_resource_graph_native_consumer_ready)) goto done;
            int renderer = sh_host_is_vulkan();
            sh_native_resource_registry_source identities = {NULL, pr_native_read,
                glb_resolve((const uint8_t *)GetModuleHandleW(NULL), "decl_resource_head", NULL)};
            if (renderer < 0 || !identities.head_slot || !sh_package_native_assets_configure_shader_catalog(
                native_assets, renderer ? SH_PACKAGE_SHADER_VULKAN : SH_PACKAGE_SHADER_OPENGL,
                identities, doom_base, error, sizeof(error))) goto done;
        }
        wchar_t language[260];
        char bank_prefix[MAX_PATH];
        int audio_ready = sh_audio_files_native_language(language,260);
        if (!sh_audio_files_native_bank_prefix(bank_prefix,sizeof(bank_prefix))) bank_prefix[0] = 0;
        audio_originals = sh_audio_originals_open(doom_base,audio_ready ? language : NULL,bank_prefix);
        if (!audio_originals) goto done;
    }
#endif
#ifdef SH_PACKAGE_RUNTIME_TESTING
    if (g_test_empty_catalog) { baseline = g_test_baseline; baseline_context = g_test_baseline_context; }
    if (g_test_audio_root) {
        audio_originals = sh_audio_originals_open(g_test_audio_root,g_test_audio_language,
            g_test_audio_bank_prefix);
        if (!audio_originals) goto done;
    }
#endif
    environment.baseline = baseline; environment.baseline_context = baseline_context;
    environment.baseline_identity = sh_audio_originals_identity; environment.identity_context = audio_originals;
    environment.produce = pr_produce; environment.canonical_path = pr_canonical;
    environment.previous = previous ? previous->compiled : NULL;
    environment.builtins = builtins;
    environment.builtin_count = sizeof(builtins) / sizeof(builtins[0]);
    for (default_index = 0; default_index < environment.builtin_count; default_index++) {
        const ov_baked_decl_t *value = &g_ov_baked_decls[default_index];
        builtins[default_index] = (sh_package_builtin){value->name, (const unsigned char *)value->text, value->len};
    }
#ifdef SH_PACKAGE_RUNTIME_TESTING
    /* This opt-in fixture models only its registered declaration classes.
     * Production and default runtime fixtures retain every product builtin. */
    if (sh_package_runtime_test_source_inputs(NULL, NULL, NULL, NULL)) environment.builtin_count = 0;
#endif
    if (!source_root && !product_only) {
        environment.invalid_package = &invalid_package;
        environment.unserved_package = &unserved_package;
        environment.conflicted_packages = &conflicted;
    }
    if (inventory_out) {
        sh_package_sources empty = {0};
        inventory_sources = sh_package_sources_join(sources, &empty, error, sizeof(error));
        if (!inventory_sources) goto done;
    }
    if (authoring_increment) {
        int linked_sources = reflection != 0;
#ifdef SH_PACKAGE_RUNTIME_TESTING
        linked_sources = sh_package_runtime_test_source_inputs(NULL, &reflection, NULL, NULL);
#endif
        if (linked_sources) {
            /* Keep the complete captured inventory above. Only newly captured
             * local trees need authoring output; old immutable native bindings
             * will be copied into the resulting library without recompilation.
             * Folder equality prevents a different delivery with equal modern
             * bytes from being mistaken for this existing installed owner. */
            for (size_t i = sources->package_count; i; i--) {
                int present = 0;
                for (size_t j = 0; previous && j < previous->sources->package_count; j++)
                    if (!strcmp(sources->packages[i - 1].name, previous->sources->packages[j].name) &&
                        !memcmp(sources->fingerprints[i - 1], previous->sources->fingerprints[j], 32)) {
                        present = 1; break;
                    }
                if (present && !sh_package_sources_remove(sources, i - 1)) goto done;
            }
            for (size_t i = 0; i < sources->component_count; i++) {
                const sh_package_component *component = sources->components + i;
                const unsigned char *tree = sh_package_source_tree(sources, component->owner);
                if (!tree || sh_package_provenance_context_add(&authoring_contexts,
                    SH_PACKAGE_CONTEXT_ISOLATED, (const unsigned char (*)[32])tree, 1,
                    component->relative) == SIZE_MAX) goto done;
            }
            contexts = authoring_contexts.context_count ? &authoring_contexts : NULL;
            environment.previous = NULL;
            environment.invalid_package = NULL; environment.unserved_package = NULL;
            environment.conflicted_packages = NULL;
            if (!contexts) environment.builtin_count = 0;
        }
    }
    if (source_root && !contexts && sources->package_count
#ifdef SH_PACKAGE_RUNTIME_TESTING
        && sh_package_runtime_test_source_inputs(NULL, NULL, NULL, NULL)
#endif
    ) {
        sh_package_compile_environment old_environment = environment;
        /* Prior providers contain private linked/restoration entries. They are
         * not part of this carrier's original historical composition. */
        old_environment.previous = NULL;
        compiled = sh_package_compile_with(sources, &old_environment, error, sizeof(error));
        if (!compiled) { if (composition_failed) *composition_failed = 1; goto done; }
        if (!pr_legacy_bindings(compiled, catalog, &historical) ||
            (!historical.context_count && sh_package_provenance_context_add(&historical,
                SH_PACKAGE_CONTEXT_COMPOSED, sources->original_fingerprints ?
                    sources->original_fingerprints : sources->fingerprints,
                sources->package_count, NULL) == SIZE_MAX)) {
            snprintf(error, sizeof(error), "cannot retain the map's historical source context"); goto done;
        }
        historical.semantics = SH_PACKAGE_PROVENANCE_HISTORICAL;
        sh_package_compilation_free(compiled); compiled = NULL;
        contexts = &historical;
    }
    for (;;) {
        if (contexts
#ifndef SH_PACKAGE_RUNTIME_TESTING
            || (!source_root && !product_only && sources->component_count)
#else
            || (g_test_installed_links && !source_root && !product_only && sources->component_count)
#endif
        ) {
            sh_decl_registry_source registry = {0};
            pr_original_source originals = {baseline, baseline_context, native_assets};
            sh_package_library_native native = sh_package_native_assets_adapter(native_assets);
#ifndef SH_PACKAGE_RUNTIME_TESTING
            native.private_keys = sh_spawn_keys_active();
#endif
            /* Installed packages keep the registration keys the installed
             * library gave them: a map compile keys installed scopes as that
             * library does, and a library recompile keeps its live keys. */
            {
                const pr_provider *keys = source_root ? string_peer : previous;
                native.keys = keys ? &keys->bindings : NULL;
                native.key_groups = keys ? keys->key_groups : NULL;
            }
            /* Audio compiles into one retained program per provider. Native
             * activation publishes that program; there is no old-path fallback. */
            native.audio = audio_originals;
#ifndef SH_PACKAGE_RUNTIME_TESTING
            /* One pending namespace includes the retained peer provider before
             * assigning this candidate's private string identities. */
            sh_strids_reservation_free(strings);
            strings = sh_strids_reservation_open(error, sizeof(error));
            if (!strings || (string_peer && !sh_strids_reservation_policy(strings,
                &string_peer->compiled->policy, error, sizeof(error)))) goto done;
            native.strings = sh_strids_reservation_adapter(strings, 65536);
            native.identities = (sh_native_resource_registry_source){NULL, pr_native_read,
                glb_resolve((const uint8_t *)GetModuleHandleW(NULL), "decl_resource_head", NULL)};
            if (!native.identities.head_slot) {
                snprintf(error, sizeof(error), "native resource identities are unavailable at package compilation");
                goto done;
            }
            if (!sh_decl_server_registry_source(&registry)) {
                snprintf(error, sizeof(error), "native source registry is unavailable at package compilation");
                goto done;
            }
#else
            if (!sh_package_runtime_test_source_inputs(&registry, &reflection, NULL, NULL)) {
                snprintf(error, sizeof(error), "native map bindings require registered source readers"); goto done;
            }
            native.source.implicit = g_test_constructor ? pr_test_constructor : NULL;
#endif
            ULONGLONG started = GetTickCount64();
            char scope_error[512], line[1280];
            sh_package_native_assets_shader_scope_begin(native_assets);
            int linked = contexts ? sh_package_library_compile_contexts(sources, contexts, &environment,
                catalog, pr_original, &originals, registry, reflection, &native, &isolated, error, sizeof(error)) :
                sh_package_library_compile(sources, &environment, catalog, pr_original, &originals,
                    registry, reflection, &native, &isolated, error, sizeof(error));
            if (!sh_package_native_assets_shader_scope_end(native_assets, scope_error, sizeof(scope_error)) && linked) {
                /* Native identities changed under this candidate: environmental. */
                sh_package_library_free(&isolated); linked = 0;
                strcpy_s(error, sizeof(error), scope_error);
            }
            snprintf(line, sizeof(line), "package library compile (%s): %s in %llu ms%s%s",
                authoring_increment ? "local authoring" : contexts ? "map contexts" : "installed library",
                linked ? "linked" : "failed",
                (unsigned long long)(GetTickCount64() - started), linked ? "" : ": ", linked ? "" : error);
            backend_log(line);
            char phases[256];
            snprintf(line, sizeof(line), "package library phases: %s%s%s%s%s%s%s",
                sh_package_library_phases(&isolated, phases, sizeof(phases)),
                isolated.source_phases[0] ? " (source link: " : "", isolated.source_phases,
                isolated.source_phases[0] ? ")" : "",
                isolated.contribution_phases[0] ? " (contributions: " : "", isolated.contribution_phases,
                isolated.contribution_phases[0] ? ")" : "");
            backend_log(line);
            /* Accounted coverage gaps help diagnose a later refusal too. */
            if (!linked && isolated.coverage) backend_log(isolated.coverage);
            if (linked) {
                compiled = isolated.compiled; isolated.compiled = NULL; library_output = 1;
            }
        } else
            compiled = sh_package_compile_with(sources, &environment, error, sizeof(error));
        if (compiled) break;
#ifndef SH_PACKAGE_RUNTIME_TESTING
        if (sh_package_native_assets_shader_pending(native_assets)) {
            /* A missing native construction receipt is environmental work,
             * never malformed source or a conflict between package owners. */
            if (!pr_prepare_native_sources(native_assets, &prepared_tasks, &prepared_task_count,
                error, sizeof(error))) goto done;
            sh_package_library_free(&isolated);
            invalid_package = unserved_package = SIZE_MAX; sh_package_owners_free(&conflicted);
            error[0] = 0;
            continue;
        }
#endif
        if (unserved_package != SIZE_MAX) {
            if (unserved_package >= sources->package_count) goto done;
            /* Its files are valid and stay available to map installation;
             * healthy peers compile without it. */
            if (!pr_unserved(&unserved, &sources->packages[unserved_package], error) ||
                !sh_package_sources_remove(sources, unserved_package)) goto done;
            continue;
        }
        if (invalid_package != SIZE_MAX) {
            if (invalid_package >= sources->package_count) goto done;
            /* Invalid content cannot establish installed availability. Valid
             * peers that only disagree retain their physical inventory: a
             * map's authored policy may use these files without reinstalling. */
            if (inventory_sources) for (size_t i = 0; i < inventory_sources->package_count; i++)
                if (!strcmp(inventory_sources->packages[i].name, sources->packages[invalid_package].name)) {
                    if (!sh_package_sources_remove(inventory_sources, i)) goto done;
                    break;
                }
            if (!pr_rejected(&rejections, &sources->packages[invalid_package], error) ||
                !sh_package_sources_remove(sources, invalid_package)) goto done;
            continue;
        }
        if (!sh_package_owners_count(&conflicted)) break;
        for (size_t i = sources->package_count; i > 0; i--) if (sh_package_owners_contains(&conflicted, i - 1)) {
            if (!pr_rejected(&rejections, &sources->packages[i - 1], error) ||
                !sh_package_sources_remove(sources, i - 1)) goto done;
        }
    }
    if (inventory_out) {
        /* The inventory has its own diagnostic: a composition failure above
         * remains the reported cause when the inventory itself succeeds. */
        char inventory_error[2048] = "";
        *inventory_out = pr_inventory_build(inventory_sources, inventory_error, sizeof(inventory_error));
        inventory_sources = NULL;
        if (!*inventory_out) {
            strcpy_s(error, sizeof(error), inventory_error[0] ? inventory_error : "cannot retain the installed package inventory");
            goto done;
        }
    }
    if (!compiled) { if (composition_failed) *composition_failed = 1; goto done; }
    if (!sh_package_sound_prepare(compiled, error, sizeof(error))) goto done;
#ifndef SH_PACKAGE_RUNTIME_TESTING
    if (sh_package_sound_requires_binding(compiled) && !sh_package_sound_native_ready()) {
        snprintf(error, sizeof(error), "native sound event binding is unavailable at package compilation");
        goto done;
    }
    if (strings && !sh_strids_reservation_policy(strings, &compiled->policy, error, sizeof(error))) goto done;
    if (isolated.audio) {
        char directory[4096];
        if (!pr_cache_directory(data_root, directory, sizeof(directory))) {
            snprintf(error, sizeof(error), "cannot prepare compiled audio cache"); goto done;
        }
        if (!sh_audio_banks_native_reserve(isolated.audio, compiled, audio_originals,
                &audio_ids, error, sizeof(error)) ||
            !sh_package_audio_program_materialize(isolated.audio, &compiled, directory,
                error, sizeof(error))) goto done;
    }
#endif
    if (source_root && !contexts && !pr_legacy_bindings(compiled, catalog, &isolated.bindings)) {
        snprintf(error, sizeof(error), "cannot retain the map's historical source context"); goto done;
    }
    /* A linked library holds every scope's complete closure; its unchanged
     * SnapMap originals stay native rather than being published again. */
    if (library_output) {
        size_t native = sh_package_compilation_prune_native(compiled,
            catalog ? pr_snapmap_serves : NULL, (void *)catalog);
        char line[160];
        snprintf(line, sizeof(line), "package library: %zu published resources; %zu unchanged SnapMap resources stay native",
            compiled->resource_count, native);
        backend_log(line);
    }
    if (!pr_cache_resources(compiled, data_root, error, sizeof(error))) goto done;
    provider = calloc(1, sizeof(*provider));
    if (!provider) goto done;
    /* Remember where this compilation was scanned from so the library can be
     * recompiled later without the caller supplying the root again. */
    if (data_root && !source_root && !(provider->data_root = _strdup(data_root))) {
        free(provider); provider = NULL; goto done;
    }
    provider->references = 1;
    if (use_deliveries) {
        provider->bundles = original_bundles; original_bundles = NULL;
        provider->bundle_count = delivery_count;
    } else if (!pr_capture_bundles(provider, sources, data_root, error, sizeof(error))) {
        pr_release(provider); provider = NULL; goto done;
    }
    provider->sources = sources; provider->compiled = compiled; provider->catalog = catalog;
    provider->bindings = isolated.bindings; memset(&isolated.bindings, 0, sizeof(isolated.bindings));
    provider->key_groups = isolated.key_groups; isolated.key_groups = NULL;
    provider->historical_bindings = historical; memset(&historical, 0, sizeof(historical));
    provider->virtual_materials = isolated.virtual_materials; isolated.virtual_materials = NULL;
    provider->consumers = isolated.consumers; isolated.consumers = NULL;
    provider->audio = isolated.audio; isolated.audio = NULL;
    provider->audio_originals = audio_originals; audio_originals = NULL;
    provider->native_assets = native_assets; native_assets = NULL;
#ifndef SH_PACKAGE_RUNTIME_TESTING
    provider->strings = strings; strings = NULL;
    provider->audio_ids = audio_ids; audio_ids = NULL;
#endif
    provider->references = 1;
    provider->rejections = rejections; rejections = NULL;
    provider->unserved = unserved; unserved = NULL;
    provider->coverage = isolated.coverage; isolated.coverage = NULL;
    if (provider->coverage) backend_log(provider->coverage);
    sources = NULL; compiled = NULL; catalog = NULL;
done:
    if (!provider) snprintf(out_error, out_capacity, "%s", error[0] ? error : "package compilation could not be prepared");
    if (original_bundles) {
        for (size_t i = 0; i < delivery_count; i++) sh_package_bundle_release(original_bundles[i]);
        free(original_bundles);
    }
    sh_package_compilation_free(compiled); sh_package_sources_free(sources); sh_resource_catalog_close(catalog);
#ifndef SH_PACKAGE_RUNTIME_TESTING
    sh_audio_id_lease_release(audio_ids);
#endif
    sh_package_library_free(&isolated);
    sh_audio_originals_close(audio_originals);
    sh_package_native_assets_close(native_assets);
    sh_decl_native_schema_close(native_schema);
    sh_package_sources_free(inventory_sources); sh_package_owners_free(&conflicted);
    sh_package_provenance_free(&historical);
    sh_package_provenance_free(&authoring_contexts);
#ifndef SH_PACKAGE_RUNTIME_TESTING
    sh_strids_reservation_free(strings);
    free(prepared_tasks);
#endif
    free(rejections); free(unserved);
    return provider;
}

static pr_provider *pr_prepare(const char *data_root, const char *source_root,
    const pr_provider *previous, int product_only, pr_inventory **inventory_out,
    int *composition_failed, const sh_package_provenance *contexts,
    const pr_provider *string_peer, char *error, size_t capacity)
{
    return pr_prepare_deliveries(data_root, source_root, previous, product_only,
        0, inventory_out, composition_failed, contexts, NULL, 0, 0, string_peer, error, capacity);
}

static int pr_context_equal(const sh_package_provenance_context *a,
    const sh_package_provenance_context *b)
{
    return a->mode == b->mode && a->tree_count == b->tree_count &&
        !memcmp(a->trees, b->trees, a->tree_count * 32u) &&
        !strcmp(a->component ? a->component : "", b->component ? b->component : "");
}
static void pr_lease_retain(void *context)
{
    pr_occurrence_lease *lease = context;
    if (InterlockedIncrement(&lease->references) == 1) InterlockedIncrement(&g_lease_generation);
}
static void pr_lease_release(void *context)
{
    pr_occurrence_lease *lease = context;
    /* Native destructors may hold the occurrence tracker lock. No provider
     * lock, allocation, native retirement, or source release belongs here. */
    if (!InterlockedDecrement(&lease->references)) InterlockedIncrement(&g_lease_generation);
}
static int pr_binding_context(const sh_package_provenance *catalog,
    const sh_package_provenance_binding *binding, const sh_package_provenance_context *context)
{
    for (size_t i = 0; i < catalog->binding_count; i++) {
        const sh_package_provenance_binding *candidate = catalog->bindings + i;
        if (strcmp(binding->family, candidate->family) || strcmp(binding->original, candidate->original) ||
            strcmp(binding->name, candidate->name)) continue;
        for (size_t c = 0; c < candidate->context_count; c++)
            if (pr_context_equal(context, catalog->contexts + candidate->contexts[c])) return 1;
    }
    return 0;
}
static int pr_lease_binding(const pr_occurrence_lease *lease,
    const sh_package_provenance_binding *binding)
{
    return !strcmp(lease->family, binding->family) && !strcmp(lease->original, binding->original) &&
        !strcmp(lease->name, binding->name);
}
/* Called with a stable provider under g_refresh_lock. Preparation, not a native
 * copy/destructor callback. A snapshot attaches every exact selected pair. */
static int pr_capture_leases(const pr_provider *provider, sh_package_occurrence_snapshot *snapshot,
    char *error, size_t capacity)
{
    const sh_package_provenance *uses = sh_package_occurrence_provenance(snapshot);
    int ok = 0;
    if (!provider || !uses || uses->semantics != SH_PACKAGE_PROVENANCE_USES) goto failed;
    AcquireSRWLockExclusive(&g_lease_lock);
    for (size_t u = 0; u < uses->use_count; u++) {
        const sh_package_provenance_use *use = uses->uses + u;
        const sh_package_provenance_binding *binding = uses->bindings + use->binding;
        for (size_t c = 0; c < use->context_count; c++) {
            size_t index = use->contexts[c];
            const sh_package_provenance_context *context = uses->contexts + index;
            const pr_provider *owner = provider;
            pr_occurrence_lease *lease;
            if (!pr_binding_context(&owner->bindings, binding, context)) {
                owner = NULL;
                for (lease = g_leases; lease; lease = lease->next)
                    if (InterlockedCompareExchange(&lease->references, 0, 0) > 0 &&
                        pr_context_equal(context, lease->context.contexts) &&
                        pr_lease_binding(lease, binding) &&
                        pr_binding_context(&lease->provider->bindings, binding, context)) {
                        owner = lease->provider; break;
                    }
            }
            if (!owner) goto unlock;
            for (lease = g_leases; lease; lease = lease->next)
                if (lease->provider == owner && pr_context_equal(context, lease->context.contexts) &&
                    pr_lease_binding(lease, binding)) break;
            if (!lease) {
                lease = calloc(1, sizeof(*lease));
                if (!lease) goto unlock;
                if (sh_package_provenance_context_add(&lease->context, context->mode,
                    context->trees, context->tree_count, context->component) == SIZE_MAX) { free(lease); goto unlock; }
                lease->family = _strdup(binding->family); lease->original = _strdup(binding->original); lease->name = _strdup(binding->name);
                if (!lease->family || !lease->original || !lease->name) {
                    free(lease->family); free(lease->original); free(lease->name);
                    sh_package_provenance_free(&lease->context); free(lease); goto unlock;
                }
                lease->provider = pr_retain((pr_provider *)owner); lease->next = g_leases; g_leases = lease;
            }
            if (!sh_package_occurrence_bind_resource(snapshot, use->binding, index,
                (sh_package_occurrence_resource){lease, pr_lease_retain, pr_lease_release})) goto unlock;
        }
    }
    ok = 1;
unlock:
    ReleaseSRWLockExclusive(&g_lease_lock);
failed:
    if (!ok && error && capacity) snprintf(error, capacity, "selected occurrence has no exact immutable native provider lease");
    return ok;
}
int sh_package_runtime_bind_occurrence_resources(const sh_package_map_plan *plan,
    sh_package_occurrence_snapshot *snapshot, char *error, size_t capacity)
{
    int ok;
    AcquireSRWLockShared(&g_refresh_lock); AcquireSRWLockShared(&g_lock);
    ok = pr_capture_leases(plan ? plan->provider : g_current_provider, snapshot, error, capacity);
    ReleaseSRWLockShared(&g_lock); ReleaseSRWLockShared(&g_refresh_lock); return ok;
}
int sh_package_runtime_occurrences_pending(void)
{
    return InterlockedCompareExchange(&g_lease_generation, 0, 0) !=
        InterlockedCompareExchange(&g_lease_published, 0, 0);
}
sh_package_selector_hold *sh_package_runtime_selectors_retain(char *error, size_t capacity)
{
    sh_package_selector_hold *hold = NULL;
    if (error && capacity) *error = 0;
    if (!g_activation_callback) AcquireSRWLockExclusive(&g_refresh_lock);
    AcquireSRWLockShared(&g_lock);
    if (!g_current_provider || (!g_usable && !g_activation_callback)) goto done;
    hold = calloc(1, sizeof(*hold));
    if (!hold) goto done;
    hold->provider = pr_retain(g_current_provider); hold->live = 1;
    hold->next = g_selectors; g_selectors = hold;
    InterlockedIncrement(&g_lease_generation);
done:
    ReleaseSRWLockShared(&g_lock);
    if (!g_activation_callback) ReleaseSRWLockExclusive(&g_refresh_lock);
    if (!hold && error && capacity) snprintf(error, capacity, "native selectors require a retained published resource view");
    return hold;
}
void sh_package_runtime_selectors_release(sh_package_selector_hold *hold)
{
    /* Native retirement is the caller's prior obligation. Destruction here
     * only schedules serving cleanup; it never frees a resource under native
     * palette code or requires the provider publication lock. */
    if (hold && InterlockedExchange(&hold->live, 0)) InterlockedIncrement(&g_lease_generation);
}
/* Successful activation only, with g_refresh_lock exclusive. A candidate and
 * its copied immutable payload already own everything still being served. */
static void pr_collect_leases(void)
{
    pr_occurrence_lease **slot = &g_leases;
    while (*slot) {
        pr_occurrence_lease *lease = *slot;
        if (InterlockedCompareExchange(&lease->references, 0, 0)) { slot = &lease->next; continue; }
        *slot = lease->next; pr_release(lease->provider);
        free(lease->family); free(lease->original); free(lease->name);
        sh_package_provenance_free(&lease->context); free(lease);
    }
    sh_package_selector_hold **selector = &g_selectors;
    while (*selector) {
        sh_package_selector_hold *hold = *selector;
        if (InterlockedCompareExchange(&hold->live, 0, 0)) { selector = &hold->next; continue; }
        *selector = hold->next; pr_release(hold->provider); free(hold);
    }
}

static int pr_lease_context_live(const pr_provider *provider, const sh_package_provenance_context *context,
    const sh_package_provenance_binding *binding)
{
    for (const pr_occurrence_lease *lease = g_leases; lease; lease = lease->next)
        if (lease->provider == provider && InterlockedCompareExchange((volatile LONG *)&lease->references, 0, 0) > 0 &&
            pr_context_equal(context, lease->context.contexts) && pr_lease_binding(lease, binding)) return 1;
    return 0;
}
static int pr_context_selected(const sh_package_provenance *uses,
    const sh_package_provenance_context *context)
{
    for (size_t u = 0; uses && u < uses->use_count; u++)
        for (size_t c = 0; c < uses->uses[u].context_count; c++)
            if (pr_context_equal(context, uses->contexts + uses->uses[u].contexts[c])) return 1;
    return 0;
}
static int pr_lease_owner(const pr_provider *provider, size_t owner,
    const sh_package_provenance *selected)
{
    const unsigned char *tree = sh_package_source_tree(provider->sources, owner);
    if (selected) {
        for (size_t c = 0; tree && c < selected->context_count; c++)
            if (pr_context_selected(selected, selected->contexts + c))
                for (size_t t = 0; t < selected->contexts[c].tree_count; t++)
                    if (!memcmp(tree, selected->contexts[c].trees[t], 32)) return 1;
        return 0;
    }
    for (const pr_occurrence_lease *lease = g_leases; tree && lease; lease = lease->next)
        if (lease->provider == provider && InterlockedCompareExchange((volatile LONG *)&lease->references, 0, 0) > 0)
            for (size_t t = 0; t < lease->context.contexts[0].tree_count; t++)
                if (!memcmp(tree, lease->context.contexts[0].trees[t], 32)) return 1;
    return 0;
}
static int pr_promoted_owner(const sh_package_sources *sources, size_t owner, const sh_package_provenance *promoted)
{
    const unsigned char *tree = sh_package_source_tree(sources, owner);
    for (size_t u = 0; tree && promoted && u < promoted->use_count; u++) {
        const sh_package_provenance_use *use = promoted->uses + u;
        for (size_t c = 0; c < use->context_count; c++) {
            const sh_package_provenance_context *context = promoted->contexts + use->contexts[c];
            for (size_t t = 0; t < context->tree_count; t++) if (!memcmp(tree, context->trees[t], 32)) return 1;
        }
    }
    return 0;
}
static int pr_lease_resource(const pr_provider *provider, const sh_compiled_resource *resource,
    const sh_package_provenance *selected)
{
    for (size_t b = 0; b < provider->bindings.binding_count; b++) {
        const sh_package_provenance_binding *binding = provider->bindings.bindings + b;
        if (!sh_package_resource_native_matches(resource, binding->family, binding->name)) continue;
        if (selected) {
            for (size_t u = 0; u < selected->use_count; u++) {
                const sh_package_provenance_use *use = selected->uses + u;
                const sh_package_provenance_binding *wanted = selected->bindings + use->binding;
                if (strcmp(wanted->family, binding->family) || strcmp(wanted->original, binding->original) ||
                    strcmp(wanted->name, binding->name)) continue;
                for (size_t c = 0; c < use->context_count; c++)
                    if (pr_binding_context(&provider->bindings, wanted, selected->contexts + use->contexts[c])) return 1;
            }
            continue;
        }
        for (size_t c = 0; c < binding->context_count; c++)
            if (pr_lease_context_live(provider, provider->bindings.contexts + binding->contexts[c], binding)) return 1;
    }
    return 0;
}
typedef struct pr_retained_selection {
    const pr_provider *provider;
    unsigned char *resources;
    sh_package_owners owners;
} pr_retained_selection;
enum { PR_RETAIN_OCCURRENCES, PR_RETAIN_AUTHORING, PR_RETAIN_SELECTORS };

static int pr_retained_select(const pr_provider *provider, const sh_package_provenance *selected, int mode,
    pr_retained_selection *out,
    char *error, size_t capacity)
{
    size_t count = provider->compiled->resource_count, head = 0, tail = 0;
    size_t *queue = NULL;
    int ok = 0;
    out->provider = provider;
    if (count > SIZE_MAX / sizeof(*queue)) goto done;
    out->resources = calloc(count ? count : 1, 1);
    queue = malloc((count ? count : 1) * sizeof(*queue));
    if (!out->resources || !queue) goto done;
    for (size_t i = 0; i < provider->sources->package_count; i++)
        if ((mode != PR_RETAIN_OCCURRENCES || pr_lease_owner(provider, i, selected)) &&
            !sh_package_owners_add(&out->owners, i)) goto done;
    for (size_t i = 0; i < count; i++) if (mode == PR_RETAIN_SELECTORS || (mode == PR_RETAIN_AUTHORING ?
        sh_package_owners_count(&provider->compiled->resources[i].owners) != 0 :
        pr_lease_resource(provider, provider->compiled->resources + i, selected))) {
        out->resources[i] = 1; queue[tail++] = i;
    }
    while (head < tail) {
        const sh_compiled_resource *resource = provider->compiled->resources + queue[head++];
        if (!sh_package_owners_union(&out->owners, &resource->owners) ||
            !sh_package_owners_union(&out->owners, &resource->gameplay_owners)) goto done;
        if (resource->source != SIZE_MAX && (resource->source >= provider->sources->file_count ||
            !sh_package_owners_add(&out->owners, provider->sources->files[resource->source].owner))) goto done;
        for (size_t f = 0; f < resource->source_count; f++)
            if (resource->sources[f] >= provider->sources->file_count || !sh_package_owners_add(&out->owners,
                provider->sources->files[resource->sources[f]].owner)) goto done;
        for (size_t d = 0; d < resource->bound_dependency_count; d++) {
            const sh_compiled_resource *target = sh_package_compilation_find(provider->compiled, resource->bound_dependencies[d]);
            if (!target) {
                snprintf(error, capacity, "%s: retained bound dependency is absent: %s",
                    resource->engine_path, resource->bound_dependencies[d]); goto done;
            }
            size_t index = (size_t)(target - provider->compiled->resources);
            if (!out->resources[index]) { out->resources[index] = 1; queue[tail++] = index; }
        }
    }
    ok = 1;
done:
    free(queue);
    if (!ok) { free(out->resources); out->resources = NULL; sh_package_owners_free(&out->owners); }
    return ok;
}
static int pr_lease_material(void *context, const char *name)
{
    const pr_retained_selection *selection = context;
    const pr_provider *provider = selection->provider;
    for (size_t i = 0; i < provider->compiled->resource_count; i++) {
        const sh_compiled_resource *resource = provider->compiled->resources + i;
        if (resource->type && !strcmp(resource->type, "material") && !strcmp(resource->name, name))
            return selection->resources[i];
    }
    return 0;
}
static int pr_lease_consumer(void *context, const char *family, const char *name)
{
    const pr_retained_selection *selection = context;
    const sh_package_compilation *compiled = selection->provider->compiled;
    for (size_t i = 0; i < compiled->resource_count; i++)
        if (selection->resources[i] && sh_package_resource_native_matches(compiled->resources + i, family, name)) return 1;
    return 0;
}
static int pr_retained_policy_sources(sh_package_sources *sources, const sh_package_sources *authored,
    size_t retained_begin, const sh_package_provenance *promoted, const sh_package_provenance *admitted)
{
    for (size_t c = 0; c < sources->component_count; c++) {
        sh_package_component *component = sources->components + c;
        if (component->owner < retained_begin) continue;
        if (pr_promoted_owner(sources, component->owner, promoted) ||
            pr_promoted_owner(sources, component->owner, admitted)) {
            sh_json_object_free(&component->descriptor.fields);
            const sh_json_object *fields = &authored->components[c].descriptor.fields;
            for (size_t f = 0; f < fields->count; f++)
                if (!sh_json_object_set(&component->descriptor.fields, fields->members[f].key,
                    fields->members[f].value_json, 16)) return 0;
        } else if (!sh_json_object_set(&component->descriptor.fields, "requirements", "{}", 16) ||
            !sh_json_object_set(&component->descriptor.fields, "strings", "{}", 16) ||
            !sh_json_object_set(&component->descriptor.fields, "hud", "{}", 16)) return 0;
    }
    return 1;
}
static int pr_resource_equal(const sh_package_compilation *a, const sh_compiled_resource *ar,
    const sh_package_compilation *b, const sh_compiled_resource *br, char *error, size_t capacity);
/* A verified unchanged SnapMap reference names the native original slot, not
 * an authored editor variant. Local collection extensions may already occupy
 * that same slot. Their presence cannot turn a vanilla dependency into an
 * instruction to replace the current editor metadata. Authored changes have
 * no native_original certificate and must retain their private exact bytes. */
static int pr_original_editor_slot(const sh_compiled_resource *current,
    const sh_compiled_resource *original)
{
    return current && original && original->native_original && original->baseline_known == 1 &&
        !current->original_path && !original->original_path && current->type && original->type &&
        !strcmp(current->type, original->type) && !sh_package_owners_count(&current->gameplay_owners) &&
        !sh_package_owners_count(&original->gameplay_owners) &&
        (!strcmp(original->type, "entitydef") || !strncmp(original->type, "snapeditor", 10) ||
            !strncmp(original->type, "snappropertyinspector", 21));
}

/* The immutable origin catalog supplies the context. Served byte equality and
 * its resolved dependency closure prove eligibility; coalesced owner lists do
 * not establish which source a newly authored native reference belongs to. */
static int pr_binding_served(const pr_provider *origin, const sh_package_compilation *serving,
    const sh_package_provenance_binding *binding, char *error, size_t capacity)
{
    size_t count = origin->compiled->resource_count, head = 0, tail = 0;
    unsigned char *seen = NULL;
    size_t *queue = NULL;
    int result = -1;
    if (count > SIZE_MAX / sizeof(*queue)) goto done;
    seen = calloc(count ? count : 1, 1); queue = malloc((count ? count : 1) * sizeof(*queue));
    if (!seen || !queue) goto done;
    for (size_t i = 0; i < count; i++) {
        const sh_compiled_resource *resource = origin->compiled->resources + i;
        if (sh_package_resource_native_matches(resource, binding->family, binding->name)) {
            const sh_compiled_resource *current = sh_package_compilation_find(serving, resource->engine_path);
            if (!current || !sh_package_resource_native_matches(current, binding->family, binding->name)) {
                result = 0; goto done;
            }
            seen[i] = 1; queue[tail++] = i;
        }
    }
    if (!tail) { result = 0; goto done; }
    while (head < tail) {
        const sh_compiled_resource *resource = origin->compiled->resources + queue[head++];
        const sh_compiled_resource *current = sh_package_compilation_find(serving, resource->engine_path);
        if (!current) { result = 0; goto done; }
        int equal = pr_resource_equal(serving, current, origin->compiled, resource, error, capacity);
        if (equal != 1 && (equal < 0 || !pr_original_editor_slot(current, resource))) { result = equal; goto done; }
        for (size_t i = 0; i < resource->bound_dependency_count; i++) {
            const sh_compiled_resource *target = sh_package_compilation_find(origin->compiled, resource->bound_dependencies[i]);
            if (!target) { result = 0; goto done; }
            size_t index = (size_t)(target - origin->compiled->resources);
            if (!seen[index]) { seen[index] = 1; queue[tail++] = index; }
        }
    }
    result = 1;
done:
    free(seen); free(queue); return result;
}
static int pr_context_bundles(const sh_package_provenance_context *context,
    sh_package_bundle *const *bundles, size_t count)
{
    for (size_t t = 0; t < context->tree_count; t++) {
        size_t i;
        for (i = 0; i < count; i++) {
            const unsigned char *tree = sh_package_bundle_fingerprint(bundles[i]);
            if (tree && !memcmp(tree, context->trees[t], 32)) break;
        }
        if (i == count) return 0;
    }
    return 1;
}
typedef struct pr_served_binding_filter {
    const pr_provider *origin;
    const sh_package_compilation *serving;
    char *error;
    size_t capacity;
} pr_served_binding_filter;
static int pr_binding_still_served(void *context, const char *family, const char *name)
{
    pr_served_binding_filter *filter = context;
    for (size_t i = 0; i < filter->origin->bindings.binding_count; i++) {
        const sh_package_provenance_binding *binding = filter->origin->bindings.bindings + i;
        if (!strcmp(binding->family, family) && !strcmp(binding->name, name))
            return pr_binding_served(filter->origin, filter->serving, binding, filter->error, filter->capacity);
    }
    return 0;
}
static int pr_retained_bindings(pr_provider *view, const pr_provider *provider,
    const sh_package_compilation *serving, sh_package_bundle *const *bundles, size_t bundle_count,
    char *error, size_t capacity)
{
    for (size_t b = 0; b < provider->bindings.binding_count; b++) {
        const sh_package_provenance_binding *binding = provider->bindings.bindings + b;
        int served = pr_binding_served(provider, serving, binding, error, capacity);
        if (served < 0) return 0;
        if (!served) continue;
        for (size_t c = 0; c < binding->context_count; c++) {
            const sh_package_provenance_context *context = provider->bindings.contexts + binding->contexts[c];
            if (!pr_context_bundles(context, bundles, bundle_count)) continue;
            /* Current map bindings remain authoritative for newly selected
             * values. Old exact native names are retained by the lease itself. */
            int occupied = 0;
            for (size_t i = 0; i < view->bindings.binding_count && !occupied; i++) {
                const sh_package_provenance_binding *current = view->bindings.bindings + i;
                if (strcmp(current->family, binding->family) || strcmp(current->original, binding->original)) continue;
                for (size_t k = 0; k < current->context_count; k++)
                    if (pr_context_equal(context, view->bindings.contexts + current->contexts[k])) occupied = 1;
            }
            if (occupied) continue;
            size_t index = sh_package_provenance_context_add(&view->bindings, context->mode,
                context->trees, context->tree_count, context->component);
            if (index == SIZE_MAX || !sh_package_provenance_bind(&view->bindings,
                binding->family, binding->original, binding->name, &index, 1)) return 0;
        }
    }
    return 1;
}
static int pr_resource_equal(const sh_package_compilation *a, const sh_compiled_resource *ar,
    const sh_package_compilation *b, const sh_compiled_resource *br, char *error, size_t capacity)
{
    sh_package_compilation av = *a, bv = *b;
    sh_package_changes changes = {0};
    av.resources = (sh_compiled_resource *)ar; av.resource_count = 1;
    bv.resources = (sh_compiled_resource *)br; bv.resource_count = 1;
    int ok = sh_package_compilation_changes(&av, &bv, &changes, error, capacity);
    int equal = ok && !changes.count; sh_package_changes_free(&changes); return ok ? equal : -1;
}
/* Active native capture only. Portable map rebinding still resolves through
 * the prepared context catalog. An old spelling is authoritative here only
 * while every selected source has a live lease and every bound native file is
 * still served with those exact retained bytes. */
static int pr_retained_use(void *context, const sh_package_provenance *saved,
    size_t index, const char **replacement)
{
    const pr_provider *current = context;
    if (replacement) *replacement = NULL;
    if (!current || !saved || saved->semantics != SH_PACKAGE_PROVENANCE_USES ||
        index >= saved->use_count || !replacement) return -1;
    const sh_package_provenance_use *use = saved->uses + index;
    if (use->binding >= saved->binding_count || !use->context_count) return -1;
    const sh_package_provenance_binding *binding = saved->bindings + use->binding;
    for (size_t c = 0; c < use->context_count; c++) {
        if (use->contexts[c] >= saved->context_count) return -1;
        const sh_package_provenance_context *required = saved->contexts + use->contexts[c];
        int found = 0;
        for (const pr_occurrence_lease *lease = g_leases; lease; lease = lease->next) {
            if (InterlockedCompareExchange((volatile LONG *)&lease->references, 0, 0) <= 0 ||
                !pr_context_equal(required, lease->context.contexts) ||
                !pr_lease_binding(lease, binding) ||
                !pr_binding_context(&lease->provider->bindings, binding, required)) continue;
            char error[512];
            if (pr_binding_served(lease->provider, current->compiled, binding, error, sizeof(error)) != 1) return -1;
            found = 1; break;
        }
        if (!found) return 0;
    }
    *replacement = binding->name; return 1;
}
static int pr_remap_owners(sh_package_owners *out, const sh_package_owners *owners,
    const size_t *map, size_t count)
{
    for (size_t i = 0; i < count; i++) if (map[i] != SIZE_MAX &&
        sh_package_owners_contains(owners, i) && !sh_package_owners_add(out, map[i])) return 0;
    return 1;
}
/* Add one provider's live contexts using owned bytes/handles only. A complete
 * source is copied once per exact original+compiler tree in the resulting view;
 * equal resources keep independent occurrence provenance and source ownership. */
static int pr_view_retained(pr_provider *view, const pr_provider *provider,
    size_t retained_begin, const sh_package_provenance *promoted,
    const sh_package_provenance *admitted, int mode, int *metadata_needed, char *error, size_t capacity)
{
    const sh_package_sources *originals = provider->authored_sources ? provider->authored_sources : provider->sources;
    sh_package_sources empty = {0}, *extra = NULL, *sources = NULL, *authored = NULL;
    sh_package_compilation *compiled = NULL;
    sh_package_resource_projection *projections = NULL;
    sh_compiled_resource *remapped = NULL;
    const sh_compiled_resource **inputs = NULL;
    sh_package_bundle **bundles = NULL;
    sh_virtual_material_view *filtered = NULL, *materials = NULL;
    sh_package_consumers *selected_consumers = NULL, *consumers = NULL;
    pr_retained_selection selected = {0};
    size_t *owners = NULL, *files = NULL;
    size_t old_owners = view->sources->package_count, old_files = view->sources->file_count;
    size_t maximum = view->compiled->resource_count + provider->compiled->resource_count, count = 0, next_owner = old_owners;
    int ok = 0, deferred_shared = 0;
    if (maximum < view->compiled->resource_count || maximum > SIZE_MAX / (2u * sizeof(*inputs)) ||
        provider->sources->package_count > SIZE_MAX / sizeof(*owners) ||
        provider->sources->file_count > SIZE_MAX / sizeof(*files)) goto done;
    if (!pr_retained_select(provider, admitted, mode, &selected, error, capacity)) goto done;
    owners = malloc((provider->sources->package_count ? provider->sources->package_count : 1) * sizeof(*owners));
    files = malloc((provider->sources->file_count ? provider->sources->file_count : 1) * sizeof(*files));
    extra = sh_package_sources_join(originals, &empty, error, capacity);
    if (!owners || !files || !extra) goto done;
    for (size_t i = 0; i < provider->sources->package_count; i++) {
        owners[i] = SIZE_MAX;
        if (!sh_package_owners_contains(&selected.owners, i)) continue;
        for (size_t j = 0; j < old_owners; j++)
            if (!memcmp(sh_package_source_tree(provider->sources, i), sh_package_source_tree(view->sources, j), 32) &&
                !memcmp(provider->sources->fingerprints[i], view->sources->fingerprints[j], 32)) { owners[i] = j; break; }
        if (owners[i] == SIZE_MAX) owners[i] = next_owner++;
    }
    for (size_t i = provider->sources->package_count; i; i--)
        if (owners[i - 1] == SIZE_MAX || owners[i - 1] < old_owners)
            if (!sh_package_sources_remove(extra, i - 1)) goto done;
    sources = sh_package_sources_join(view->sources, extra, error, capacity);
    authored = sh_package_sources_join(view->authored_sources ? view->authored_sources : view->sources,
        extra, error, capacity);
    if (!sources || !authored || (mode != PR_RETAIN_AUTHORING &&
        !pr_retained_policy_sources(sources, authored, retained_begin, promoted, admitted))) goto done;
    size_t new_file = old_files;
    for (size_t i = 0; i < provider->sources->file_count; i++) {
        const sh_package_source_file *file = provider->sources->files + i;
        size_t owner = owners[file->owner]; files[i] = SIZE_MAX;
        if (owner == SIZE_MAX) continue;
        if (owner >= old_owners) { files[i] = new_file++; continue; }
        for (size_t j = 0; j < old_files; j++) {
            const sh_package_source_file *candidate = sources->files + j;
            if (candidate->owner == owner && !strcmp(candidate->relative, file->relative) &&
                candidate->length == file->length && !memcmp(candidate->digest, file->digest, 32)) { files[i] = j; break; }
        }
        if (files[i] == SIZE_MAX) goto done;
    }
    projections = calloc(maximum ? maximum : 1, sizeof(*projections));
    inputs = calloc(maximum ? maximum * 2 : 1, sizeof(*inputs));
    remapped = calloc(provider->compiled->resource_count ? provider->compiled->resource_count : 1, sizeof(*remapped));
    if (!projections || !inputs || !remapped) goto done;
    for (size_t i = 0; i < view->compiled->resource_count; i++) {
        const sh_compiled_resource *resource = view->compiled->resources + i;
        inputs[count * 2] = resource;
        projections[count++] = (sh_package_resource_projection){resource->engine_path, inputs + i * 2, 1,
            NULL, 0, resource->native_type, resource->native_name};
    }
    for (size_t i = 0; i < provider->compiled->resource_count; i++) {
        const sh_compiled_resource *resource = provider->compiled->resources + i;
        if (!selected.resources[i]) continue;
        const sh_compiled_resource *existing = sh_package_compilation_find(view->compiled, resource->engine_path);
        size_t at = existing ? (size_t)(existing - view->compiled->resources) : count;
        if (existing) {
            int equal = pr_resource_equal(view->compiled, existing, provider->compiled, resource, error, capacity);
            if (equal < 0) goto done;
            if (!equal) {
                if (mode == PR_RETAIN_SELECTORS) {
                    /* The old native palette still borrows this slot. Keep
                     * its exact meaning for the full palette replacement
                     * pass; releasing the selector schedules the next update.
                     * A map's explicit gameplay binding cannot be substituted. */
                    if (view->compiled->map_overlay && !sh_package_owners_within(
                        &existing->gameplay_owners, view->compiled->map_owner_begin)) {
                        snprintf(error, capacity, "%s: a native selector still holds a different map binding",
                            resource->engine_path); goto done;
                    }
                    projections[at].input_count = 0;
                    projections[at].native_type = resource->native_type;
                    projections[at].native_name = resource->native_name;
                    deferred_shared = 1;
                } else {
                    if (pr_original_editor_slot(existing, resource)) continue;
                    if (!resource->original_path && !sh_package_owners_count(&resource->gameplay_owners) && resource->type &&
                        (!strcmp(resource->type, "entitydef") || !strncmp(resource->type, "snapeditor", 10) ||
                            !strncmp(resource->type, "snappropertyinspector", 21))) {
                        if (!metadata_needed) continue;
                        *metadata_needed = 1;
                        snprintf(error, capacity, "%s: shared editor metadata requires additive native admission", resource->engine_path);
                        goto done;
                    }
                    snprintf(error, capacity, "%s: retained native identity has a different meaning; isolate the source context before activation",
                        resource->engine_path); goto done;
                }
            }
        }
        sh_compiled_resource *copy = remapped + i; *copy = *resource;
        copy->owners = (sh_package_owners){0}; copy->gameplay_owners = (sh_package_owners){0};
        copy->sources = NULL; copy->source_count = 0;
        copy->source = resource->source != SIZE_MAX ? files[resource->source] : SIZE_MAX;
        if (!pr_remap_owners(&copy->owners, &resource->owners, owners, provider->sources->package_count) ||
            !pr_remap_owners(&copy->gameplay_owners, &resource->gameplay_owners, owners, provider->sources->package_count)) goto done;
        if (resource->source_count && !(copy->sources = malloc(resource->source_count * sizeof(*copy->sources)))) goto done;
        for (size_t f = 0; f < resource->source_count; f++) if (files[resource->sources[f]] != SIZE_MAX)
            copy->sources[copy->source_count++] = files[resource->sources[f]];
        if (!existing) {
            projections[count] = (sh_package_resource_projection){resource->engine_path, inputs + count * 2, 0,
                NULL, 0, resource->native_type, resource->native_name}; count++;
        }
        inputs[at * 2 + projections[at].input_count++] = copy;
    }
    compiled = sh_package_compilation_project(sources, projections, count, &view->compiled->policy,
        view->compiled->canonical_path, error, capacity);
    if (!compiled) goto done;
    compiled->independent_sources = view->compiled->independent_sources;
    compiled->map_overlay = view->compiled->map_overlay ||
        (mode == PR_RETAIN_OCCURRENCES && sources->package_count > retained_begin);
    compiled->map_owner_begin = mode != PR_RETAIN_OCCURRENCES ? view->compiled->map_owner_begin :
        view->compiled->map_overlay ? view->compiled->map_owner_begin : retained_begin;
    compiled->map_independent_sources = mode != PR_RETAIN_OCCURRENCES ? view->compiled->map_independent_sources :
        view->compiled->map_overlay ? view->compiled->map_independent_sources : 1;
    for (size_t i = 0; i < provider->compiled->policy.localized_count; i++) {
        const sh_package_localized_string *row = provider->compiled->policy.localized + i;
        sh_package_owners selected = {0};
        int copied = pr_remap_owners(&selected, &row->owners, owners, provider->sources->package_count);
        if (copied && sh_package_owners_count(&selected)) copied = sh_package_policy_localization(
            &compiled->policy, row->key, row->mapping, &selected, error, capacity);
        sh_package_owners_free(&selected); if (!copied) goto done;
    }
    if (mode != PR_RETAIN_SELECTORS && provider->compiled->policy.hud_bound) compiled->policy.hud_bound = 1;
    for (size_t i = 0; mode != PR_RETAIN_SELECTORS && i < provider->compiled->policy.hud_binding_count; i++) {
        const sh_package_hud_binding *row = provider->compiled->policy.hud_bindings + i;
        sh_package_owners selected = {0};
        int copied = pr_remap_owners(&selected, &row->owners, owners, provider->sources->package_count);
        if (copied && sh_package_owners_count(&selected)) copied = sh_package_hud_bind(
            &compiled->policy, row->name, row->weapon_mode, &selected);
        sh_package_owners_free(&selected); if (!copied) goto done;
    }
    for (size_t i = 0; i < provider->sources->package_count; i++)
        if (owners[i] != SIZE_MAX && (mode == PR_RETAIN_AUTHORING || pr_promoted_owner(sources, owners[i], promoted) ||
            pr_promoted_owner(sources, owners[i], admitted)) &&
            sh_package_owners_contains(&provider->compiled->policy.legacy_string_owners, i) &&
            !sh_package_owners_add(&compiled->policy.legacy_string_owners, owners[i])) goto done;
    sh_package_policy policy = {0}; sh_package_owners all = {0};
    for (size_t i = 0; i < sources->package_count; i++) if (!sh_package_owners_add(&all, i)) {
        sh_package_owners_free(&all); goto done;
    }
    int policy_ok = sh_package_compilation_policy(compiled, &all, &policy, error, capacity);
    sh_package_owners_free(&all); if (!policy_ok) goto done;
    sh_package_policy_free(&compiled->policy); compiled->policy = policy;
    bundles = calloc(sources->package_count ? sources->package_count : 1, sizeof(*bundles)); if (!bundles) goto done;
    for (size_t i = 0; i < old_owners; i++) bundles[i] = sh_package_bundle_retain(view->bundles[i]);
    for (size_t i = 0; i < provider->sources->package_count; i++) if (owners[i] != SIZE_MAX && owners[i] >= old_owners)
        bundles[owners[i]] = sh_package_bundle_retain(provider->bundles[i]);
    if (provider->consumers) {
        selected_consumers = sh_package_consumers_filter(provider->consumers, pr_lease_consumer, &selected, error, capacity);
        if (!selected_consumers || !(consumers = sh_package_consumers_join(view->consumers, selected_consumers, error, capacity))) goto done;
    }
    if (provider->virtual_materials) {
        filtered = sh_virtual_material_runtime_filter(provider->virtual_materials, pr_lease_material,
            &selected, error, capacity);
        if (!filtered || !(materials = sh_virtual_material_runtime_join(view->virtual_materials, filtered, error, capacity))) goto done;
    }
    if (deferred_shared) {
        sh_package_provenance next = {0};
        pr_served_binding_filter filter = {view, compiled, error, capacity};
        /* The candidate's new source cannot certify the older bytes retained
         * for a live selector. Drop unserved recipes before adding exact old
         * recipes, so capture cannot assign the pending source prematurely. */
        if (!sh_package_provenance_select(&view->bindings, pr_binding_still_served, &filter, &next)) goto done;
        next.semantics = view->bindings.semantics;
        sh_package_provenance_free(&view->bindings); view->bindings = next;
    }
    if (!pr_retained_bindings(view, provider, compiled, bundles, sources->package_count, error, capacity)) goto done;
    sh_package_compilation_free(view->compiled); view->compiled = compiled; compiled = NULL;
    sh_package_sources_free(view->sources); view->sources = sources; sources = NULL;
    sh_package_sources_free(view->authored_sources); view->authored_sources = authored; authored = NULL;
    for (size_t i = 0; i < view->bundle_count; i++) sh_package_bundle_release(view->bundles[i]);
    free(view->bundles); view->bundles = bundles; bundles = NULL; view->bundle_count = view->sources->package_count;
    if (materials) { sh_virtual_material_runtime_retire(&view->virtual_materials); view->virtual_materials = materials; materials = NULL; }
    if (consumers) { sh_package_consumers_free(view->consumers); view->consumers = consumers; consumers = NULL; }
    ok = 1;
done:
    if (remapped) for (size_t i = 0; i < provider->compiled->resource_count; i++) {
        free(remapped[i].sources); sh_package_owners_free(&remapped[i].owners); sh_package_owners_free(&remapped[i].gameplay_owners);
    }
    if (bundles) { for (size_t i = 0; sources && i < sources->package_count; i++) sh_package_bundle_release(bundles[i]); free(bundles); }
    sh_virtual_material_runtime_retire(&filtered); sh_virtual_material_runtime_retire(&materials);
    sh_package_consumers_free(selected_consumers); sh_package_consumers_free(consumers);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources); sh_package_sources_free(authored);
    sh_package_sources_free(extra); free(owners); free(files); free(projections); free(inputs); free(remapped);
    free(selected.resources); sh_package_owners_free(&selected.owners);
    if (!ok && error && capacity && !*error) snprintf(error, capacity, "cannot preserve immutable occurrence resource contexts");
    return ok;
}

static pr_provider *pr_view(pr_provider *library, const pr_provider *map,
    const sh_package_provenance *promoted, char *error, size_t capacity)
{
    sh_package_sources empty_sources = {0};
    sh_package_compilation empty = {0};
    pr_provider *view = calloc(1, sizeof(*view));
    if (!view) return NULL;
    view->references = 1;
    if (promoted && promoted->semantics == SH_PACKAGE_PROVENANCE_USES &&
        !sh_package_provenance_select_uses(promoted, NULL, NULL, &view->policy_uses)) { pr_release(view); return NULL; }
    empty.sources = &empty_sources; empty.canonical_path = library->compiled->canonical_path;
    view->compiled = sh_package_compilation_overlay(library->compiled, map ? map->compiled : &empty, error, capacity);
    if (!view->compiled) { pr_release(view); return NULL; }
    /* The view provider takes the independent inventory from the compiler. */
    view->sources = (sh_package_sources *)view->compiled->sources;
    view->compiled->owns_sources = 0;
    if (!map) { view->compiled->map_overlay = 0; view->compiled->map_owner_begin = 0; }
    view->parent = pr_retain(library); view->catalog = library->catalog;
    view->audio_originals = library->audio_originals;
    view->native_assets = library->native_assets;
    if (!pr_audio_source_add(view, map) || !pr_audio_source_add(view, library)) {
        snprintf(error, capacity, "cannot retain active audio programs"); pr_release(view); return NULL;
    }
    if (!pr_view_bundles(view, library, map, error, capacity)) { pr_release(view); return NULL; }
    if (!sh_package_provenance_overlay(&library->bindings, map ? &map->bindings : NULL, &view->bindings)) {
        snprintf(error, capacity, "cannot retain map source bindings"); pr_release(view); return NULL;
    }
    if ((library->consumers || (map && map->consumers)) && !(view->consumers =
        sh_package_consumers_join(library->consumers, map ? map->consumers : NULL, error, capacity))) { pr_release(view); return NULL; }
    if (library->virtual_materials || (map && map->virtual_materials)) {
        view->virtual_materials = sh_virtual_material_runtime_join(library->virtual_materials,
            map ? map->virtual_materials : NULL, error, capacity);
        if (!view->virtual_materials) { pr_release(view); return NULL; }
    }
    size_t retained_begin = view->sources->package_count;
    for (const pr_occurrence_lease *lease = g_leases; lease; lease = lease->next) {
        if (InterlockedCompareExchange((volatile LONG *)&lease->references, 0, 0) <= 0) continue;
        const pr_occurrence_lease *previous;
        for (previous = g_leases; previous != lease; previous = previous->next)
            if (previous->provider == lease->provider && InterlockedCompareExchange((volatile LONG *)&previous->references, 0, 0) > 0) break;
        if (previous != lease) continue;
        if (!pr_view_retained(view, lease->provider, retained_begin, promoted, NULL,
            PR_RETAIN_OCCURRENCES, NULL, error, capacity) || !pr_audio_source_add(view, lease->provider)) {
            if (!*error) snprintf(error, capacity, "cannot retain active audio programs");
            pr_release(view); return NULL;
        }
    }
    for (const sh_package_selector_hold *hold = g_selectors; hold; hold = hold->next) {
        if (!InterlockedCompareExchange((volatile LONG *)&hold->live, 0, 0)) continue;
        const sh_package_selector_hold *prior;
        for (prior = g_selectors; prior != hold; prior = prior->next)
            if (prior->provider == hold->provider && InterlockedCompareExchange((volatile LONG *)&prior->live, 0, 0)) break;
        if (prior != hold) continue;
        if (!pr_view_retained(view, hold->provider, retained_begin, promoted, NULL,
            PR_RETAIN_SELECTORS, NULL, error, capacity) || !pr_audio_source_add(view, hold->provider)) {
            if (!*error) snprintf(error, capacity, "cannot retain active audio programs");
            pr_release(view); return NULL;
        }
    }
#ifndef SH_PACKAGE_RUNTIME_TESTING
    if (view->compiled->policy.localized_count) {
        view->strings = sh_strids_reservation_open(error, capacity);
        if (!view->strings || !sh_strids_reservation_policy(view->strings,
            &view->compiled->policy, error, capacity)) { pr_release(view); return NULL; }
    }
#endif
    return view;
}

/* Clone the published view directly. Rebuilding library+map here could retire
 * a released clipboard lease or reinterpret current shared metadata while the
 * editor still owns native consumers. Additive admission changes neither. */
static pr_provider *pr_clone_serving(pr_provider *base, char *error, size_t capacity)
{
    sh_package_sources empty = {0};
    sh_package_resource_projection *rows = NULL;
    const sh_compiled_resource **inputs = NULL;
    pr_provider *view = calloc(1, sizeof(*view));
    if (!view) return NULL;
    view->references = 1; view->parent = pr_retain(base);
    view->catalog = base->catalog; view->audio_originals = base->audio_originals;
    view->native_assets = base->native_assets;
    if (!pr_audio_source_add(view, base)) goto failed;
    if (base->data_root && !(view->data_root = _strdup(base->data_root))) goto failed;
    view->sources = sh_package_sources_join(base->sources, &empty, error, capacity);
    if (!view->sources) goto failed;
    if (base->authored_sources) {
        view->authored_sources = sh_package_sources_join(base->authored_sources, &empty, error, capacity);
        if (!view->authored_sources) goto failed;
    }
    size_t count = base->compiled->resource_count;
    if (count > SIZE_MAX / sizeof(*rows) || count > SIZE_MAX / sizeof(*inputs)) goto failed;
    rows = calloc(count ? count : 1, sizeof(*rows)); inputs = calloc(count ? count : 1, sizeof(*inputs));
    if (!rows || !inputs) goto failed;
    for (size_t i = 0; i < count; i++) {
        const sh_compiled_resource *r = base->compiled->resources + i; inputs[i] = r;
        rows[i] = (sh_package_resource_projection){r->engine_path, inputs + i, 1,
            NULL, 0, r->native_type, r->native_name};
    }
    view->compiled = sh_package_compilation_project(view->sources, rows, count,
        &base->compiled->policy, base->compiled->canonical_path, error, capacity);
    if (!view->compiled || !pr_view_bundles(view, base, NULL, error, capacity) ||
        !sh_package_provenance_overlay(&base->bindings, NULL, &view->bindings)) goto failed;
    view->compiled->map_overlay = base->compiled->map_overlay;
    view->compiled->map_owner_begin = base->compiled->map_owner_begin;
    view->compiled->independent_sources = base->compiled->independent_sources;
    view->compiled->map_independent_sources = base->compiled->map_independent_sources;
    if (base->policy_uses.semantics == SH_PACKAGE_PROVENANCE_USES &&
        !sh_package_provenance_select_uses(&base->policy_uses, NULL, NULL, &view->policy_uses)) goto failed;
    if (base->virtual_materials && !(view->virtual_materials =
        sh_virtual_material_runtime_join(base->virtual_materials, NULL, error, capacity))) goto failed;
    if (base->consumers && !(view->consumers = sh_package_consumers_join(base->consumers, NULL, error, capacity))) goto failed;
    free(rows); free(inputs); return view;
failed:
    free(rows); free(inputs); pr_release(view); return NULL;
}

/* A prepared source cannot be smuggled into the current provider by presenting
 * another document's aliases. Every use must still have its exact plan lease. */
static int pr_plan_occurrences(const pr_provider *plan, const sh_package_provenance *uses,
    const sh_package_compilation *serving, char *error, size_t capacity)
{
    if (!plan || !uses || uses->semantics != SH_PACKAGE_PROVENANCE_USES) goto failed;
    for (size_t u = 0; u < uses->use_count; u++) {
        const sh_package_provenance_use *use = uses->uses + u;
        if (use->binding >= uses->binding_count || !use->context_count) goto failed;
        const sh_package_provenance_binding *binding = uses->bindings + use->binding;
        for (size_t c = 0; c < use->context_count; c++) {
            if (use->contexts[c] >= uses->context_count) goto failed;
            const sh_package_provenance_context *source = uses->contexts + use->contexts[c];
            if (!pr_binding_context(&plan->bindings, binding, source) || !pr_lease_context_live(plan, source, binding)) goto failed;
        }
        if (serving && pr_binding_served(plan, serving, binding, error, capacity) != 1) goto failed;
    }
    return 1;
failed:
    if (error && capacity && !*error) snprintf(error, capacity, "prepared occurrences do not retain their exact admitted source resources");
    return 0;
}

static int pr_exact_policy(const pr_provider *provider, const sh_package_provenance *uses,
    const char *json, size_t length,
    const sh_package_references *references, sh_package_policy *out, sh_package_owners *owners,
    char *error, size_t capacity)
{
    if (!uses || uses->semantics != SH_PACKAGE_PROVENANCE_USES)
        return sh_package_map_policy(provider->compiled, provider->catalog, json, length, references,
            out, owners, NULL, error, capacity);
    sh_package_owners observed = {0}, selected = {0};
    sh_package_compilation authored = *provider->compiled;
    authored.sources = provider->authored_sources ? provider->authored_sources : provider->sources;
    authored.policy.legacy_string_owners = (sh_package_owners){0};
    int ok = sh_package_map_owners(provider->compiled, provider->catalog, json, length, references, &observed, NULL);
    for (size_t i = 0; ok && i < provider->sources->package_count; i++)
        if (sh_package_owners_contains(&observed, i) && pr_promoted_owner(provider->sources, i, uses))
            ok = sh_package_owners_add(&selected, i);
    for (size_t i = 0; ok && i < provider->sources->package_count; i++) {
        if (!sh_package_owners_contains(&selected, i)) continue;
        const unsigned char *tree = sh_package_source_tree(provider->sources, i);
        int historical = sh_package_owners_contains(&provider->compiled->policy.legacy_string_owners, i);
        for (size_t c = 0; c < uses->context_count && !historical; c++) if (uses->contexts[c].mode == SH_PACKAGE_CONTEXT_COMPOSED)
            for (size_t t = 0; t < uses->contexts[c].tree_count; t++) if (!memcmp(tree, uses->contexts[c].trees[t], 32)) historical = 1;
        if (historical) ok = sh_package_owners_add(&authored.policy.legacy_string_owners, i);
    }
    if (ok) ok = sh_package_compilation_policy(&authored, &selected, out, error, capacity);
    if (owners) { sh_package_owners_free(owners); *owners = selected; memset(&selected, 0, sizeof(selected)); }
    sh_package_owners_free(&authored.policy.legacy_string_owners);
    sh_package_owners_free(&observed); sh_package_owners_free(&selected); return ok;
}
static int pr_selected_policy(const pr_provider *provider, const char *json, size_t length,
    const sh_package_references *references, sh_package_policy *out, sh_package_owners *owners,
    char *error, size_t capacity)
{
    return pr_exact_policy(provider, &provider->policy_uses, json, length, references, out, owners, error, capacity);
}

/* The caller owns g_refresh_lock. Selection can invalidate a retained proof
 * under g_lock, but cannot replace its paths during this transaction. */
static int pr_required_available(const pr_provider *map, const pr_provider *library,
    const pr_inventory *inventory, char *error, size_t capacity)
{
    sh_package_missing missing = {0};
    pr_availability availability = {library, inventory};
    const char **paths = NULL;
    int ok = 0, current_use;
    AcquireSRWLockShared(&g_lock);
    current_use = map->requirements_known != 0;
    if (current_use && map == g_map_provider && g_map_json) {
        sh_package_file_identity identity;
        current_use = g_map_length == map->required_map.length &&
            sh_package_bytes_identity(g_map_json, g_map_length, &identity) &&
            !memcmp(identity.digest, map->required_map.digest, sizeof(identity.digest));
    }
    ReleaseSRWLockShared(&g_lock);
    if (!current_use) {
        snprintf(error, capacity, "Map resource coverage is incomplete: inspect the current map use before continuing its provider");
        return 0;
    }
    if (map->required_paths.count) {
        if (map->required_paths.count > SIZE_MAX / sizeof(*paths) ||
            !(paths = malloc(map->required_paths.count * sizeof(*paths)))) {
            snprintf(error, capacity, "cannot retain required map resource paths"); return 0;
        }
        for (size_t i = 0; i < map->required_paths.count; i++) paths[i] = map->required_paths.items[i].name;
    }
    ok = sh_package_compilation_missing(map->compiled, paths, map->required_paths.count,
        pr_inventory_available, &availability, &missing, error, capacity);
    if (ok && missing.count) {
        snprintf(error, capacity, "%s: installed packages no longer supply a required map resource", missing.paths[0]);
        ok = 0;
    }
    free(paths); sh_package_missing_free(&missing); return ok;
}

static int pr_change_order(const void *a, const void *b)
{ return strcmp(((const sh_package_change *)a)->path, ((const sh_package_change *)b)->path); }
static int pr_identity_order(const void *a, const void *b)
{
    const sh_package_native_identity *x = a, *y = b;
    int order = strcmp(x->type, y->type); return order ? order : strcmp(x->name, y->name);
}
/* A native constructor can change without changing a cooked file. Keep the
 * complete logical identity set separate from file rows: several managers can
 * share one path, and verified engine-created owners may have no file at all. */
static int pr_constructor_changes(const sh_package_consumers *before, const sh_package_consumers *after,
    sh_package_changes *changes)
{
    size_t ac = sh_package_consumers_count(before), bc = sh_package_consumers_count(after);
    if (changes->rebound || changes->rebound_count || bc > SIZE_MAX - ac ||
        ac + bc > SIZE_MAX / sizeof(*changes->rebound)) return 0;
    if (!ac && !bc) return 1;
    changes->rebound = calloc(ac + bc, sizeof(*changes->rebound)); if (!changes->rebound) return 0;
    for (int side = 0; side < 2; side++) {
        const sh_package_consumers *view = side ? after : before;
        for (size_t i = 0; i < sh_package_consumers_count(view); i++) {
            const sh_package_consumer *owner = sh_package_consumers_at(view, i);
            if ((side && sh_package_consumers_find(before, owner->family, owner->name)) ||
                sh_package_consumers_equal(before, after, owner->family, owner->name)) continue;
            sh_package_native_identity key = {(char *)owner->family, (char *)owner->name};
            /* Additions/removals already carry constructor/retirement work.
             * A rebound is specifically a surviving identity in this diff. */
            if ((changes->added_count && bsearch(&key, changes->added, changes->added_count, sizeof(key), pr_identity_order)) ||
                (changes->removed_count && bsearch(&key, changes->removed, changes->removed_count, sizeof(key), pr_identity_order))) continue;
            sh_package_native_identity *row = changes->rebound + changes->rebound_count++;
            row->type = _strdup(owner->family); row->name = _strdup(owner->name);
            if (!row->type || !row->name) return 0;
        }
    }
    qsort(changes->rebound, changes->rebound_count, sizeof(*changes->rebound), pr_identity_order); return 1;
}
#ifdef SH_PACKAGE_RUNTIME_TESTING
int sh_package_runtime_test_consumer_changes(const sh_package_consumers *before,
    const sh_package_consumers *after, sh_package_changes *changes)
{ return pr_constructor_changes(before, after, changes); }
#endif
static int pr_material_changed(const pr_provider *before, const pr_provider *after,
    const sh_compiled_resource *resource, const sh_package_changes *changes, size_t original_count,
    int retry)
{
    sh_package_change key = {(char *)resource->engine_path, 0};
    if (!resource->type || !resource->name || strcmp(resource->type, "material")) return 0;
    if (sh_virtual_material_runtime_binding_equal(before ? before->virtual_materials : NULL,
        after->virtual_materials, resource->name) &&
        !(retry && before && !sh_virtual_material_runtime_binding_equal(
            before->virtual_materials, NULL, resource->name))) return 0;
    return !original_count || !bsearch(&key, changes->items, original_count,
        sizeof(key), pr_change_order);
}
/* A material can have identical source bytes while acquiring/removing a private
 * native atlas binding. Include that native reconstruction in the same resource
 * diff, otherwise an already-loaded object would retain its previous mapping. */
static int pr_native_changes(const pr_provider *before, const pr_provider *after,
    sh_package_changes *changes, char *error, size_t capacity)
{
    size_t extra = 0, original_count = changes->count;
    const sh_package_compilation *compiled = after->compiled;
    int retry = before && !sh_virtual_material_runtime_healthy(before->virtual_materials);
    sh_package_change *grown;
    if (!pr_constructor_changes(before ? before->consumers : NULL, after->consumers, changes)) goto failed;
    for (size_t i = 0; i < compiled->resource_count; i++)
        if (pr_material_changed(before, after, &compiled->resources[i], changes, original_count, retry)) extra++;
    if (!extra) return 1;
    if (extra > SIZE_MAX - original_count || original_count + extra > SIZE_MAX / sizeof(*grown) ||
        !(grown = realloc(changes->items, (original_count + extra) * sizeof(*grown)))) goto failed;
    changes->items = grown;
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = &compiled->resources[i];
        if (!pr_material_changed(before, after, resource, changes, original_count, retry)) continue;
        sh_package_change *change = &changes->items[changes->count++];
        *change = (sh_package_change){_strdup(resource->engine_path), SH_PACKAGE_RESOURCE_REPLACED,
            _strdup(resource->type), _strdup(resource->name), _strdup(resource->type), _strdup(resource->name)};
        if (!change->path || !change->type || !change->name || !change->previous_type || !change->previous_name) goto failed;
    }
    qsort(changes->items, changes->count, sizeof(*changes->items), pr_change_order); return 1;
failed:
    if (error && capacity) snprintf(error, capacity, "cannot prepare native resource binding changes");
    sh_package_changes_free(changes); return 0;
}

static int pr_update(const char *data_root, int change_map, const char *map_source_root,
    const sh_package_map_plan *prepared, const sh_package_provenance *promotion,
    sh_package_activation_guard guard, sh_package_activation_fn activate, void *context)
{
    pr_provider *library = NULL, *map = NULL, *current = NULL;
    pr_provider *old_library, *old_map, *old_current;
    pr_inventory *inventory = NULL, *old_inventory = NULL;
    sh_package_policy selected = {0}, old_selected;
    sh_package_provenance promoted = {0};
    sh_package_changes changes = {0};
    sh_virtual_material_view *previous_virtual_materials = NULL;
    char *retired_map_json = NULL;
    size_t retired_map_length = 0;
    sh_package_references retired_references = {0};
    pr_original_source originals = {0};
    char error[2048] = "", line[512];
    char source_error[2048] = "";
    int ok = 0, guarded = 0, composition_failed = 0, old_usable = 0, library_rescanned = 0;
    LONG lease_generation, promotion_generation;
    AcquireSRWLockExclusive(&g_refresh_lock);
    if (g_native_preparing || g_occurrence_recovery_provider) {
        /* A concurrent/reentrant refresh cannot replace this immutable retry
         * or mark its still-valid provider failed. The owning preparation
         * decides readiness once its native recovery has completed. */
        ReleaseSRWLockExclusive(&g_refresh_lock);
        return 0;
    }
    lease_generation = InterlockedCompareExchange(&g_lease_generation, 0, 0);
    AcquireSRWLockShared(&g_lock);
    promotion_generation = g_promotion_generation;
    const sh_package_provenance *wanted_promotion = change_map == 1 ? NULL : promotion ? promotion : &g_promoted;
    int promotion_ok = !wanted_promotion || wanted_promotion->semantics != SH_PACKAGE_PROVENANCE_USES ||
        sh_package_provenance_select_uses(wanted_promotion, NULL, NULL, &promoted);
    ReleaseSRWLockShared(&g_lock); if (!promotion_ok) goto done;
    if ((guard.begin != NULL) != (guard.end != NULL)) {
        snprintf(error, sizeof(error), "package activation guard is incomplete"); goto done;
    }
    if (change_map) {
        if (!g_library_provider || (map_source_root && !*map_source_root)) {
            snprintf(error, sizeof(error), "map resources require an initialized local library and a valid source root"); goto done;
        }
        if (prepared && !pr_plan_library_current(prepared, error, sizeof(error))) goto done;
        library = pr_retain(g_library_provider);
        if (change_map == 2) map = pr_retain(g_map_provider);
        else if (prepared) map = pr_retain(prepared->provider);
        else if (map_source_root && !(map = pr_prepare(data_root, map_source_root,
            g_map_provider, 0, NULL, NULL, NULL, library, error, sizeof(error)))) goto done;
        /* A committed installation belongs to the persistent library, not to the
         * temporary map provider that carried it through activation. Recompile the
         * library in this same transaction, before the overlay is composed or
         * retired, so those resources are never mistaken for removals and retired
         * out from under the consumers still using them. A failed rescan leaves
         * the current provider in place: nothing is retired on a guess. */
        if (change_map == 1 && !(prepared && prepared->library && !prepared->inventory_committed) &&
            InterlockedCompareExchange(&g_sources_changed, 0, 0) && library->data_root) {
            pr_provider *rescanned = pr_prepare(library->data_root, NULL, library, 0,
                &inventory, NULL, NULL, map, error, sizeof(error));
            if (!rescanned) {
                backend_log("package library rescan for committed sources failed; the current provider is kept");
                goto done;
            }
            /* Only the map that will continue needs an availability proof.
             * An exiting provider stays owned until native activation succeeds;
             * its unused payload must never prevent leaving for vanilla. */
            if (map && !pr_required_available(map, rescanned, inventory, error, sizeof(error))) {
                pr_release(rescanned); goto done;
            }
            pr_release(library); library = rescanned; library_rescanned = 1;
        }
    } else {
        library_rescanned = 1;
        library = pr_prepare(data_root, NULL, g_library_provider, 0, &inventory,
            &composition_failed, NULL, g_map_provider, error, sizeof(error));
        if (!library) {
            if (!composition_failed || !inventory) goto done;
            strcpy_s(source_error, sizeof(source_error), error);
            if (g_library_provider) {
                /* Stored sources and successful authoring output are different
                 * facts. Preserve every active byte; make the new inventory
                 * available to independent map builds without activating a
                 * partial/conflicting local composition. */
                AcquireSRWLockExclusive(&g_lock);
                old_inventory = g_inventory; g_inventory = inventory; inventory = NULL;
                strcpy_s(g_authoring_error, sizeof(g_authoring_error), source_error);
                ReleaseSRWLockExclusive(&g_lock);
                pr_inventory_release(old_inventory); old_inventory = NULL;
                goto done;
            }
            /* First boot has no previous authoring output to preserve. Compile
             * the complete product baseline so valid map-specific packages can
             * still activate. No conflicting user subset becomes a winner. */
            library = pr_prepare(data_root, NULL, NULL, 1, NULL, NULL, NULL, g_map_provider, error, sizeof(error));
            if (!library) goto done;
        }
        map = pr_retain(g_map_provider);
    }
    /* Prepared installed authoring may serve during native map admission, but
     * the persistent library/inventory remain old until commit_map finishes
     * its disk transaction. The view retains this prospective library itself. */
    current = pr_view(prepared && prepared->library && !prepared->inventory_committed ? prepared->library : library,
        map, &promoted, error, sizeof(error));
    if (!current) goto done;
    if (prepared && !pr_plan_authoring_served(prepared, current->compiled, error, sizeof(error))) goto done;
    if (library->catalog) { originals.game = sh_resource_catalog_read_path; originals.context = library->catalog; }
    originals.native_assets = library->native_assets;
#ifdef SH_PACKAGE_RUNTIME_TESTING
    if (g_test_empty_catalog) { originals.game = g_test_baseline; originals.context = g_test_baseline_context; }
#endif
    if (!sh_package_compilation_restore_missing(current->compiled, g_compiled,
        pr_original, &originals, error, sizeof(error))) goto done;
    if (activate && !sh_package_compilation_changes(g_compiled, current->compiled,
        &changes, error, sizeof(error))) goto done;
    if (activate && !pr_native_changes(g_current_provider, current, &changes, error, sizeof(error))) goto done;
    if (guard.begin) { guard.begin(); guarded = 1; }
#ifndef SH_PACKAGE_RUNTIME_TESTING
    /* Validate the complete effective policy under the admitted activation
     * boundary. The publisher checks again before any private alias mutation. */
    if (current->strings && !sh_strids_reservation_validate(current->strings, error, sizeof(error))) goto done;
#endif
    AcquireSRWLockExclusive(&g_lock);
    if (promotion_generation != g_promotion_generation) {
        snprintf(error, sizeof(error), "map occurrence policy changed during provider preparation; retry at the next activation boundary");
        ReleaseSRWLockExclusive(&g_lock); goto done;
    }
    if (change_map && library_rescanned && map && !map->requirements_known) {
        snprintf(error, sizeof(error), "Map resource coverage changed during inventory preparation; inspect the current map use again");
        ReleaseSRWLockExclusive(&g_lock); goto done;
    }
    if (!(change_map == 1 && !map) && g_map_json &&
        !pr_selected_policy(current, g_map_json, g_map_length,
        &g_map_references, &selected, NULL, error, sizeof(error))) {
        ReleaseSRWLockExclusive(&g_lock); goto done;
    }
    old_current = g_current_provider; old_library = g_library_provider; old_map = g_map_provider;
    old_usable = g_usable;
    if (!sh_virtual_material_runtime_publish(current->virtual_materials,
        &previous_virtual_materials, error, sizeof(error))) {
        ReleaseSRWLockExclusive(&g_lock); goto done;
    }
    if (inventory) { old_inventory = g_inventory; g_inventory = inventory; }
    old_selected = g_map_policy; g_map_policy = selected; memset(&selected, 0, sizeof(selected));
    if (change_map == 1 && !map) {
        retired_map_json = g_map_json; retired_map_length = g_map_length;
        retired_references = g_map_references;
        g_map_json = NULL; g_map_length = 0; memset(&g_map_references, 0, sizeof(g_map_references));
    }
    pr_publish(current, library, map);
    g_activating = activate != NULL; g_usable = 0; g_ready = !g_activating; g_error[0] = 0;
    ReleaseSRWLockExclusive(&g_lock);
    if (guarded) { guard.end(1); guarded = 0; }
    if (activate && !pr_activate(activate, context, 0, &changes, error, sizeof(error))) {
        char recovery_error[1024] = "";
        int recovered;
        if (!error[0]) strcpy_s(error, sizeof(error), "package activation did not complete");
        if (guard.begin) { guard.begin(); guarded = 1; }
        AcquireSRWLockExclusive(&g_lock);
        sh_virtual_material_runtime_restore(previous_virtual_materials);
        selected = g_map_policy; g_map_policy = old_selected;
        if (change_map == 1 && !map) {
            g_map_json = retired_map_json; g_map_length = retired_map_length;
            g_map_references = retired_references;
            retired_map_json = NULL; memset(&retired_references, 0, sizeof(retired_references));
        }
        pr_publish(old_current, old_library, old_map);
        if (inventory) g_inventory = old_inventory;
        ReleaseSRWLockExclusive(&g_lock);
        if (guarded) { guard.end(1); guarded = 0; }
        sh_package_changes_reverse(&changes);
        recovered = pr_activate(activate, context, 1, &changes, recovery_error, sizeof(recovery_error));
        AcquireSRWLockExclusive(&g_lock); g_usable = recovered && old_usable; ReleaseSRWLockExclusive(&g_lock);
        if (!recovered) {
            char activation_error[sizeof(error)];
            strcpy_s(activation_error, sizeof(activation_error), error); backend_log(error);
            snprintf(error, sizeof(error), "%.900s; previous consumer recovery failed: %.900s", activation_error,
                recovery_error[0] ? recovery_error : "completion was not confirmed");
        }
        backend_log(recovered ? "package activation failed; previous provider restored and consumer recovery pass completed" :
            "package activation failed; previous provider restored, consumer recovery incomplete");
        goto done;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_activating = 0; g_ready = 1; g_usable = activate != NULL;
#ifdef SH_PACKAGE_RUNTIME_TESTING
    if (!g_native_bound) g_usable = 1; /* Pure filesystem/compiler host. */
#endif
    if (!change_map) strcpy_s(g_authoring_error, sizeof(g_authoring_error), source_error);
    sh_package_provenance_free(&g_promoted); g_promoted = promoted; memset(&promoted, 0, sizeof(promoted));
    ReleaseSRWLockExclusive(&g_lock);
    snprintf(line, sizeof(line), "package compiler: %zu packages, %zu components, %zu native resources; %zu identical duplicates, %zu composed declarations%s",
        current->sources->package_count, current->sources->component_count, current->compiled->resource_count,
        current->compiled->duplicate_count, current->compiled->composed_count, map ? "; temporary map provider active" : "");
    backend_log(line);
    /* Name each registration key this library moved, with its packages. */
    for (size_t i = 0; i < current->bindings.binding_count; i++) {
        const sh_package_provenance_binding *binding = current->bindings.bindings + i;
        char packages[192] = "";
        size_t used = 0;
        if (strcmp(binding->family, SH_PACKAGE_KEY_FAMILY) || !strcmp(binding->original, binding->name)) continue;
        for (size_t c = 0; c < binding->context_count && binding->contexts[c] < current->bindings.context_count; c++) {
            const sh_package_provenance_context *context = current->bindings.contexts + binding->contexts[c];
            for (size_t owner = 0; context->tree_count && owner < current->sources->package_count; owner++) {
                const unsigned char *tree = sh_package_source_tree(current->sources, owner);
                int n;
                if (!tree || memcmp(tree, context->trees[0], 32)) continue;
                n = snprintf(packages + used, sizeof(packages) - used, "%s%s", used ? ", " : "",
                    current->sources->packages[owner].name);
                if (n > 0 && (size_t)n < sizeof(packages) - used) used += (size_t)n; else packages[used] = 0;
            }
        }
        snprintf(line, sizeof(line), "%s: AI registration key %s is held as %s; its spawn requests and map uses follow it",
            used ? packages : "a package", binding->original, binding->name);
        backend_log(line);
    }
    if (library_rescanned) InterlockedExchange(&g_sources_changed, 0);
#ifndef SH_PACKAGE_RUNTIME_TESTING
    sh_strids_reservation_free(current->strings); current->strings = NULL;
    sh_strids_reservation_free(library->strings); library->strings = NULL;
    if (map) { sh_strids_reservation_free(map->strings); map->strings = NULL; }
#endif
    library = map = current = NULL;
    if (inventory) { pr_inventory_release(old_inventory); inventory = NULL; }
    pr_release(old_current); pr_release(old_library); pr_release(old_map);
    InterlockedExchange(&g_lease_published, lease_generation); pr_collect_leases();
    sh_package_policy_free(&old_selected); ok = 1;
    if (source_error[0]) { strcpy_s(error, sizeof(error), source_error); ok = 0; }
done:
    if (guarded) guard.end(0);
    if (!ok) {
        if (!error[0]) strcpy_s(error, sizeof(error), "package compilation could not be prepared");
        AcquireSRWLockExclusive(&g_lock);
        strcpy_s(g_error, sizeof(g_error), error); g_activating = 0; g_ready = 0;
        ReleaseSRWLockExclusive(&g_lock); backend_log(error);
    }
    pr_release(current); pr_release(library); pr_release(map); sh_package_policy_free(&selected);
    pr_inventory_release(inventory);
    free(retired_map_json); sh_package_references_free(&retired_references);
    sh_package_changes_free(&changes);
    sh_package_provenance_free(&promoted);
    ReleaseSRWLockExclusive(&g_refresh_lock); return ok;
}

int sh_package_runtime_refresh_activated(const char *data_root, sh_package_activation_guard guard,
    sh_package_activation_fn activate, void *context)
{
    return pr_update(data_root, 0, NULL, NULL, NULL, guard, activate, context);
}

int sh_package_runtime_activate_map(const char *data_root, const char *map_source_root,
    sh_package_activation_guard guard, sh_package_activation_fn activate, void *context)
{
    return pr_update(data_root, 1, map_source_root, NULL, NULL, guard, activate, context);
}

int sh_package_runtime_activate_occurrences(const sh_package_occurrence_snapshot *map,
    sh_package_activation_guard guard, sh_package_activation_fn activate, void *context)
{
    if (!activate) return 0;
    return pr_update(NULL, 2, NULL, NULL, map ? sh_package_occurrence_provenance(map) : NULL,
        guard, activate, context);
}

static int pr_plan_library_current(const sh_package_map_plan *plan, char *error, size_t capacity)
{
    if (!plan || plan->inventory_committed || !plan->library || g_library_provider == plan->library_base ||
        g_library_provider == plan->library) return 1;
    snprintf(error, capacity, "the installed authoring library changed after source preparation");
    return 0;
}

static int pr_plan_authoring_served(const sh_package_map_plan *plan,
    const sh_package_compilation *serving, char *error, size_t capacity)
{
    pr_retained_selection selection = {0}; int ok = 0;
    if (!plan || plan->inventory_committed || !plan->authoring) return 1;
    if (!pr_retained_select(plan->authoring, NULL, PR_RETAIN_AUTHORING, &selection, error, capacity)) return 0;
    for (size_t i = 0; i < plan->authoring->compiled->resource_count; i++) if (selection.resources[i]) {
        const sh_compiled_resource *resource = plan->authoring->compiled->resources + i;
        const sh_compiled_resource *current = sh_package_compilation_find(serving, resource->engine_path);
        int equal = current ? pr_resource_equal(serving, current, plan->authoring->compiled,
            resource, error, capacity) : 0;
        if (equal != 1 && (equal < 0 || !pr_original_editor_slot(current, resource))) {
            if (!*error) snprintf(error, capacity, "%s: installed authoring output is not admitted", resource->engine_path);
            goto done;
        }
    }
    ok = 1;
done:
    free(selection.resources); sh_package_owners_free(&selection.owners); return ok;
}

/* All fallible compilation/native/disk work precedes this pointer commit.
 * The installed modern tree and a portable plan's original archives remain
 * separate source owners even when their native bytes deduplicate. */
static void pr_commit_inventory_locked(sh_package_map_plan *plan,
    pr_provider **previous_library, pr_inventory **previous_inventory)
{
    if (plan && plan->inventory && !plan->inventory_committed) {
        *previous_inventory = g_inventory; g_inventory = pr_inventory_retain(plan->inventory);
        if (plan->library) {
            *previous_library = g_library_provider; g_library_provider = pr_retain(plan->library);
            pr_publish_key_groups();
            InterlockedExchange(&g_sources_changed, 0); g_authoring_error[0] = 0;
        }
        plan->inventory_committed = 1;
    }
}

int sh_package_runtime_admit_occurrences(sh_package_map_plan *plan,
    const sh_package_occurrence_snapshot *document, sh_package_activation_guard guard,
    sh_package_activation_fn activate, sh_package_occurrence_finalize_fn finalize,
    void *context, char *error, size_t capacity)
{
    pr_provider *candidate = NULL, *previous = NULL, *old_library = NULL;
    pr_inventory *old_inventory = NULL;
    sh_package_changes changes = {0};
    sh_virtual_material_view *previous_materials = NULL;
    const sh_package_provenance *uses = sh_package_occurrence_provenance(document);
    char fallback[2048]; int result = 0, guarded = 0, metadata_needed = 0, old_ready = 0, old_usable = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    AcquireSRWLockExclusive(&g_refresh_lock);
    if (!plan || !activate || !g_current_provider || !g_usable || g_activating || g_native_preparing ||
        (!!guard.begin != !!guard.end)) {
        snprintf(error, capacity, "additive source admission requires a stable provider and complete native activation boundary"); goto done;
    }
    if (!pr_plan_library_current(plan, error, capacity)) goto done;
    if (plan->inventory && !plan->inventory_committed && !plan->library) {
        snprintf(error, capacity, "installed authoring sources require complete scoped compilation before additive admission");
        goto done;
    }
    if (!pr_plan_occurrences(plan->provider, uses, NULL, error, capacity)) goto done;
    candidate = pr_clone_serving(g_current_provider, error, capacity);
    if (candidate && !plan->inventory_committed && plan->authoring && !pr_view_retained(candidate, plan->authoring,
        candidate->sources->package_count, NULL, NULL, PR_RETAIN_AUTHORING, &metadata_needed, error, capacity)) {
        if (metadata_needed) result = SH_PACKAGE_ADMISSION_NEEDS_METADATA;
        goto done;
    }
    if (!candidate || !pr_view_retained(candidate, plan->provider, candidate->sources->package_count,
        &g_promoted, uses, PR_RETAIN_OCCURRENCES, &metadata_needed, error, capacity)) {
        if (metadata_needed) result = SH_PACKAGE_ADMISSION_NEEDS_METADATA;
        goto done;
    }
    if (!pr_plan_occurrences(plan->provider, uses, candidate->compiled, error, capacity) ||
        !pr_plan_authoring_served(plan, candidate->compiled, error, capacity) ||
        !sh_package_compilation_changes(g_compiled, candidate->compiled, &changes, error, capacity) ||
        !pr_native_changes(g_current_provider, candidate, &changes, error, capacity)) goto done;
    for (size_t i = 0; i < changes.count; i++) if (changes.items[i].kind != SH_PACKAGE_RESOURCE_ADDED) {
        snprintf(error, capacity, "%s: additive source admission would change an existing native resource", changes.items[i].path);
        goto done;
    }
    if (changes.removed_count || changes.rebound_count) {
        snprintf(error, capacity, "additive source admission would change an existing native identity"); goto done;
    }
#ifndef SH_PACKAGE_RUNTIME_TESTING
    if (candidate->compiled->policy.localized_count) {
        candidate->strings = sh_strids_reservation_open(error, capacity);
        if (!candidate->strings || !sh_strids_reservation_policy(candidate->strings,
            &candidate->compiled->policy, error, capacity)) goto done;
    }
#endif
    if (guard.begin) { guard.begin(); guarded = 1; }
#ifndef SH_PACKAGE_RUNTIME_TESTING
    if (candidate->strings && !sh_strids_reservation_validate(candidate->strings, error, capacity)) goto done;
#endif
    AcquireSRWLockExclusive(&g_lock);
    previous = g_current_provider; old_ready = g_ready; old_usable = g_usable;
    if (!sh_virtual_material_runtime_publish(candidate->virtual_materials, &previous_materials, error, capacity)) {
        ReleaseSRWLockExclusive(&g_lock); goto done;
    }
    pr_publish(candidate, g_library_provider, g_map_provider);
    g_activating = 1; g_ready = g_usable = 0;
    ReleaseSRWLockExclusive(&g_lock);
    if (guarded) { guard.end(1); guarded = 0; }
    int native_ready = pr_activate(activate, context, 0, &changes, error, capacity);
    if (native_ready && finalize) {
        __try { native_ready = finalize(context, error, capacity) == 1; }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            native_ready = 0;
            snprintf(error, capacity, "occurrence installation finalization raised exception 0x%08lx", (unsigned long)GetExceptionCode());
        }
    }
    if (!native_ready) {
        char recovery_error[1024] = "";
        if (!*error) snprintf(error, capacity, "additive native preparation did not complete");
        if (guard.begin) { guard.begin(); guarded = 1; }
        AcquireSRWLockExclusive(&g_lock);
        sh_virtual_material_runtime_restore(previous_materials);
        pr_publish(previous, g_library_provider, g_map_provider);
        ReleaseSRWLockExclusive(&g_lock);
        if (guarded) { guard.end(1); guarded = 0; }
        sh_package_changes_reverse(&changes);
        int recovered = pr_activate(activate, context, 1, &changes, recovery_error, sizeof(recovery_error));
        AcquireSRWLockExclusive(&g_lock);
        g_activating = 0; g_ready = recovered && old_ready; g_usable = recovered && old_usable;
        if (!recovered) {
            g_occurrence_recovery_provider = pr_retain(previous);
            g_occurrence_recovery_context = context;
            g_occurrence_recovery_ready = old_ready;
            g_occurrence_recovery_usable = old_usable;
            char original[1024]; snprintf(original, sizeof(original), "%s", error);
            snprintf(error, capacity, "%.900s; additive consumer recovery failed: %.900s", original,
                *recovery_error ? recovery_error : "completion was not confirmed");
            snprintf(g_error, sizeof(g_error), "%s", error);
        }
        ReleaseSRWLockExclusive(&g_lock); previous = NULL; goto done;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_activating = 0; g_ready = old_ready; g_usable = old_usable;
    plan->occurrences_admitted = 1;
    if (finalize) pr_commit_inventory_locked(plan, &old_library, &old_inventory);
    ReleaseSRWLockExclusive(&g_lock);
#ifndef SH_PACKAGE_RUNTIME_TESTING
    sh_strids_reservation_free(candidate->strings); candidate->strings = NULL;
#endif
    candidate = NULL; pr_release(previous); previous = NULL;
    pr_inventory_release(old_inventory); pr_release(old_library);
    result = SH_PACKAGE_ADMISSION_READY;
done:
    if (guarded) guard.end(0);
    pr_release(candidate); sh_package_changes_free(&changes);
    if (!result && !*error) snprintf(error, capacity, "additive source admission could not be prepared");
    ReleaseSRWLockExclusive(&g_refresh_lock); return result;
}

int sh_package_runtime_activate_prepared_occurrences(sh_package_map_plan *plan,
    const sh_package_occurrence_snapshot *document, sh_package_activation_guard guard,
    sh_package_activation_fn activate, void *context, char *error, size_t capacity)
{
    return sh_package_runtime_admit_occurrences(plan, document, guard, activate, NULL,
        context, error, capacity);
}
int sh_package_runtime_complete_occurrence_recovery(void *context)
{
    pr_provider *retained = NULL;
    AcquireSRWLockExclusive(&g_refresh_lock);
    AcquireSRWLockExclusive(&g_lock);
    if (g_occurrence_recovery_provider && context == g_occurrence_recovery_context &&
        g_current_provider == g_occurrence_recovery_provider && !g_activating && !g_native_preparing) {
        retained = g_occurrence_recovery_provider; g_occurrence_recovery_provider = NULL;
        g_occurrence_recovery_context = NULL;
        g_ready = g_occurrence_recovery_ready; g_usable = g_occurrence_recovery_usable; g_error[0] = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);
    pr_release(retained); ReleaseSRWLockExclusive(&g_refresh_lock); return retained != NULL;
}

static sh_package_map_plan *pr_prepare_map(const char *data_root,
    const char *map_source_root, const sh_package_provenance *contexts,
    const sh_package_bundle_binding *deliveries, size_t delivery_count, int use_deliveries,
    char *error, size_t capacity)
{
    sh_package_map_plan *plan = NULL;
    char fallback[2048];
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    AcquireSRWLockExclusive(&g_refresh_lock);
    if (g_native_preparing) {
        snprintf(error, capacity, "native source preparation is in progress"); goto done;
    }
    if (!data_root || !*data_root || !map_source_root || !*map_source_root || !g_library_provider) {
        snprintf(error, capacity, "map preparation requires a local library, data root and private source root");
        goto done;
    }
    plan = calloc(1, sizeof(*plan));
    if (!plan) { snprintf(error, capacity, "cannot allocate map preparation"); goto done; }
    plan->provider = pr_prepare_deliveries(data_root, map_source_root, g_map_provider,
        0, 0, NULL, NULL, contexts, deliveries, delivery_count, use_deliveries, g_library_provider, error, capacity);
    if (!plan->provider) { free(plan); plan = NULL; }
done:
    ReleaseSRWLockExclusive(&g_refresh_lock); return plan;
}

sh_package_map_plan *sh_package_runtime_prepare_map_with_bindings(const char *data_root,
    const char *map_source_root, const sh_package_provenance *contexts, char *error, size_t capacity)
{ return pr_prepare_map(data_root, map_source_root, contexts, NULL, 0, 0, error, capacity); }

sh_package_map_plan *sh_package_runtime_prepare_map_deliveries(const char *data_root,
    const char *map_source_root, const sh_package_provenance *contexts,
    const sh_package_bundle_binding *deliveries, size_t delivery_count, char *error, size_t capacity)
{ return pr_prepare_map(data_root, map_source_root, contexts, deliveries, delivery_count, 1, error, capacity); }

sh_package_map_plan *sh_package_runtime_prepare_map(const char *data_root,
    const char *map_source_root, char *error, size_t capacity)
{ return sh_package_runtime_prepare_map_with_bindings(data_root, map_source_root, NULL, error, capacity); }

char *sh_package_map_plan_rebind(const sh_package_map_plan *plan, const sh_package_provenance *saved,
    const char *json, size_t length, size_t *output_length, char *error, size_t capacity)
{
    char *out = NULL, *bound = NULL; size_t bound_length = 0;
    sh_package_occurrence_snapshot *ownership = NULL;
    if (output_length) *output_length = 0;
    if (!plan || !json || !output_length) return NULL;
    const sh_package_provenance *recipe = saved ? saved :
        (plan->provider->historical_bindings.context_count ? &plan->provider->historical_bindings : NULL);
    bound = sh_package_runtime_capture_document(plan, recipe, NULL, 0, json, length,
        &bound_length, &ownership, NULL, NULL, error, capacity);
    /* Save the newly bound names together with the newly selected actual sites.
     * Retaining an old-name envelope would misclassify those same sites as edits
     * when attaching the native document after parsing. */
    if (bound && bound_length <= INT_MAX)
        out = sh_map_provenance_write(bound, bound_length, sh_package_occurrence_provenance(ownership),
            output_length, error, capacity);
    if (!out && error && capacity && !*error) snprintf(error, capacity, "map source bindings could not be reconstructed");
    free(bound); sh_package_occurrence_release(ownership); return out;
}

const sh_package_compilation *sh_package_map_plan_compilation(const sh_package_map_plan *plan)
{ return plan ? plan->provider->compiled : NULL; }

int sh_package_map_plan_attach_bundles(sh_package_map_plan *plan,
    const sh_package_bundle_binding *bindings, size_t count, char *error, size_t capacity)
{
    sh_package_bundle **next = NULL;
    pr_provider *provider;
    int ok = 0;
    if (error && capacity) error[0] = 0;
    AcquireSRWLockExclusive(&g_refresh_lock);
    provider = plan ? plan->provider : NULL;
    if (!provider || provider->references != 1 || provider == g_current_provider ||
        provider == g_map_provider || provider == g_library_provider ||
        count != provider->sources->package_count) goto done;
    next = pr_match_bundles(provider->sources, bindings, count, error, capacity);
    if (!next) goto done;
    /* Compilation has already bound original source identities. Replacing
     * snapshot handles afterward may not reinterpret any of those contexts. */
    for (size_t i = 0; i < count; i++)
        if (memcmp(sh_package_source_tree(provider->sources, i), sh_package_bundle_fingerprint(next[i]), 32)) goto done;
    for (size_t i = 0; i < provider->bundle_count; i++) sh_package_bundle_release(provider->bundles[i]);
    free(provider->bundles); provider->bundles = next; provider->bundle_count = count;
    next = NULL; ok = 1;
done:
    if (next) {
        for (size_t i = 0; i < count; i++) sh_package_bundle_release(next[i]);
        free(next);
    }
    if (!ok && error && capacity) snprintf(error, capacity,
        "complete original bundles must match every exact source owner of an unpublished map plan");
    ReleaseSRWLockExclusive(&g_refresh_lock); return ok;
}

int sh_package_map_plan_bind_virtual_materials(sh_package_map_plan *plan,
    const sh_virtual_material_runtime_binding *bindings, size_t count,
    char *error, size_t capacity)
{
    sh_virtual_material_view *candidate = NULL;
    int ok = 0;
    if (error && capacity) error[0] = 0;
    if (!plan) {
        if (error && capacity) snprintf(error, capacity, "native material bindings require a prepared map");
        return 0;
    }
    AcquireSRWLockExclusive(&g_refresh_lock);
    pr_provider *provider = plan->provider;
    if (!provider || provider->references != 1 || provider == g_current_provider ||
        provider == g_map_provider || provider == g_library_provider) {
        if (error && capacity) snprintf(error, capacity, "native material bindings require an unpublished private plan");
        goto done;
    }
    candidate = sh_virtual_material_runtime_prepare(bindings, count, error, capacity);
    if (!candidate) goto done;
    for (size_t i = 0; i < count; i++) {
        char *name = sh_virtual_material_name(bindings[i].name, strlen(bindings[i].name));
        int found = 0;
        if (name) for (size_t j = 0; j < provider->compiled->resource_count; j++) {
            const sh_compiled_resource *r = &provider->compiled->resources[j];
            if (r->type && r->name && !strcmp(r->type, "material") && !strcmp(r->name, name)) {
                found = 1; break;
            }
        }
        free(name);
        if (!found) {
            if (error && capacity) snprintf(error, capacity, "native atlas binding has no material in its compilation");
            goto done;
        }
    }
    /* Base provider views are never published directly; pr_view copies their
     * immutable metadata together with the effective resource overlay. */
    if (!sh_virtual_material_runtime_retire(&provider->virtual_materials)) {
        if (error && capacity) snprintf(error, capacity, "native material binding view is already active");
        goto done;
    }
    provider->virtual_materials = candidate; candidate = NULL;
    InterlockedExchange(&provider->requirements_known, 0); ok = 1;
done:
    sh_virtual_material_runtime_retire(&candidate);
    ReleaseSRWLockExclusive(&g_refresh_lock); return ok;
}

int sh_package_map_plan_prepare_inventory(sh_package_map_plan *plan,
    const char *data_root, char *error, size_t capacity)
{
    pr_inventory *inventory = NULL;
    pr_provider *checked = NULL, *library = NULL, *base = NULL;
    int ok = 0, metadata_needed = 0;
    if (!plan || !data_root || !*data_root) {
        if (error && capacity) snprintf(error, capacity, "installed inventory requires a prepared map and data root");
        return 0;
    }
    AcquireSRWLockExclusive(&g_refresh_lock);
    if (!g_library_provider || g_activating || g_native_preparing) {
        if (error && capacity) snprintf(error, capacity, "installed authoring preparation requires a stable library"); goto done;
    }
    base = pr_retain(g_library_provider);
    int composition_failed = 0;
    checked = pr_prepare_deliveries(data_root, NULL, base, 0, 1,
        &inventory, &composition_failed, NULL, NULL, 0, 0, plan->provider, error, capacity);
    if (!inventory || (!checked && !composition_failed)) {
        goto done;
    }
    /* A local authoring defect excludes only its own package, as in a library
     * rescan: the map continues and the summary keeps the reason. The caller
     * treats any message as a failed load, so none may remain. */
    if (!checked && error && capacity) {
        AcquireSRWLockExclusive(&g_lock);
        strcpy_s(g_authoring_error, sizeof(g_authoring_error), error);
        ReleaseSRWLockExclusive(&g_lock);
        error[0] = 0;
    }
    if (g_library_provider != base) {
        if (error && capacity) snprintf(error, capacity, "the installed library changed during source preparation"); goto done;
    }
    if (checked) {
        library = pr_clone_serving(base, error, capacity);
        if (!library || !pr_view_retained(library, checked, library->sources->package_count,
            NULL, NULL, PR_RETAIN_AUTHORING, &metadata_needed, error, capacity)) goto done;
    }
    pr_inventory_release(plan->inventory); plan->inventory = inventory; inventory = NULL;
    pr_release(plan->library); plan->library = library; library = NULL;
    pr_release(plan->library_base); plan->library_base = base; base = NULL;
    pr_release(plan->authoring); plan->authoring = checked; checked = NULL;
    plan->inventory_committed = 0;
    ok = 1;
done:
    pr_inventory_release(inventory); pr_release(checked); pr_release(library); pr_release(base);
    ReleaseSRWLockExclusive(&g_refresh_lock); return ok;
}

int sh_package_runtime_commit_map(sh_package_map_plan *plan,
    int (*commit)(char *error, size_t capacity), char *error, size_t capacity)
{
    pr_inventory *previous = NULL; pr_provider *previous_library = NULL;
    char fallback[512];
    int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    AcquireSRWLockExclusive(&g_refresh_lock);
    if (!commit || g_activating || g_native_preparing || !g_usable ||
        g_map_provider != (plan ? plan->provider : NULL)) {
        snprintf(error, capacity, "the prepared map is no longer active"); goto done;
    }
    if (!pr_plan_library_current(plan, error, capacity) ||
        !pr_plan_authoring_served(plan, g_compiled, error, capacity)) goto done;
    __try { ok = commit(error, capacity) == 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(error, capacity, "map installation commit raised exception 0x%08lx",
            (unsigned long)GetExceptionCode());
    }
    /* The complete installed tree remains provisional until native loading
     * succeeds and its disk transaction commits. Publication cannot allocate
     * or fail after that commit. A canceled load retains the old inventory. */
    if (ok && plan && plan->inventory) {
        AcquireSRWLockExclusive(&g_lock);
        pr_commit_inventory_locked(plan, &previous_library, &previous);
        ReleaseSRWLockExclusive(&g_lock);
    }
done:
    pr_inventory_release(previous); pr_release(previous_library);
    ReleaseSRWLockExclusive(&g_refresh_lock); return ok;
}

int sh_package_runtime_commit_occurrences(sh_package_map_plan *plan,
    const sh_package_occurrence_snapshot *document,
    int (*commit)(char *error, size_t capacity), char *error, size_t capacity)
{
    pr_inventory *previous = NULL; pr_provider *previous_library = NULL; char fallback[1024]; int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    AcquireSRWLockExclusive(&g_refresh_lock);
    if (!plan || !plan->occurrences_admitted || !commit || g_activating || g_native_preparing ||
        !g_usable || !g_current_provider || !pr_plan_occurrences(plan->provider,
            sh_package_occurrence_provenance(document), g_compiled, error, capacity)) {
        if (!*error) snprintf(error, capacity, "the prepared occurrence source is no longer admitted"); goto done;
    }
    if (!pr_plan_library_current(plan, error, capacity) ||
        !pr_plan_authoring_served(plan, g_compiled, error, capacity)) goto done;
    if (plan->inventory && !plan->inventory_committed && !plan->library) {
        snprintf(error, capacity, "installed authoring sources were not compiled for additive admission"); goto done;
    }
    __try { ok = commit(error, capacity) == 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(error, capacity, "source installation commit raised exception 0x%08lx", (unsigned long)GetExceptionCode());
    }
    if (ok && plan->inventory) {
        AcquireSRWLockExclusive(&g_lock);
        pr_commit_inventory_locked(plan, &previous_library, &previous);
        ReleaseSRWLockExclusive(&g_lock);
    }
done:
    pr_inventory_release(previous); pr_release(previous_library); ReleaseSRWLockExclusive(&g_refresh_lock); return ok;
}

static int pr_inventory_available(void *context, const char *path, char *error, size_t capacity)
{
    const pr_availability *availability = context;
    const pr_provider *library = availability->library;
    const pr_inventory *inventory = availability->inventory;
    unsigned char *original = NULL;
    size_t length = 0;
    int present, invalid = 0;
    char *canonical = pr_canonical(path);
    pr_original_source source = {sh_resource_catalog_read_path, library->catalog, library->native_assets};
    if (!canonical) {
        if (error && capacity) snprintf(error, capacity, "invalid installed resource path: %s", path);
        return -1;
    }
    size_t first = 0, last = inventory ? inventory->count : 0;
    while (first < last) {
        size_t middle = first + (last - first) / 2;
        if (strcmp(inventory->entries[middle].path, canonical) < 0) first = middle + 1;
        else last = middle;
    }
    for (size_t i = first; inventory && i < inventory->count &&
        !strcmp(inventory->entries[i].path, canonical); i++) {
        const sh_package_source_file *file = &inventory->sources->files[inventory->entries[i].source];
        if (sh_package_source_verify(file)) { free(canonical); return 1; }
        invalid = 1;
    }
    sh_package_original_identity audio_identity;
    present = sh_audio_originals_identity(library->audio_originals, canonical, &audio_identity, error, capacity);
    free(canonical);
    if (present < 0) return -1;
    if (present && audio_identity.scope) return 1;
#ifdef SH_PACKAGE_RUNTIME_TESTING
    if (g_test_empty_catalog) { source.game = g_test_baseline; source.context = g_test_baseline_context; }
#endif
    present = pr_original(&source, path, &original, &length); free(original);
    if (present == 1 || present == 3) return 1;
    if (invalid) {
        if (error && capacity) snprintf(error, capacity, "%s: installed source changed or became unreadable; refresh packages", path);
        return -1;
    }
    if (present < 0) snprintf(error, capacity, "%s: required original is unreadable or ambiguous", path);
    return present < 0 ? -1 : present == 1 || present == 3;
}

static int pr_library_available(void *context, const char *path, char *error, size_t capacity)
{
    pr_availability availability = {context, g_inventory};
    return pr_inventory_available(&availability, path, error, capacity);
}

int sh_package_map_plan_missing(const sh_package_map_plan *plan,
    const char *const *required_paths, size_t count,
    sh_package_missing *out, char *error, size_t capacity)
{
    int ok = 0;
    AcquireSRWLockShared(&g_lock);
    if (plan && g_library_provider)
        ok = sh_package_compilation_missing(plan->provider->compiled, required_paths, count,
            pr_library_available, g_library_provider, out, error, capacity);
    else {
        sh_package_missing_free(out);
        if (error && capacity) snprintf(error, capacity, "map availability requires a prepared map and local library");
    }
    ReleaseSRWLockShared(&g_lock); return ok;
}

int sh_package_map_plan_resources(const sh_package_map_plan *plan,
    const char *json, size_t length, const sh_package_references *references,
    sh_package_references *out)
{
    if (!plan) { if (out != references) sh_package_references_free(out); return 0; }
    return sh_package_map_resources(plan->provider->compiled, plan->provider->catalog,
        json, length, references, out);
}

int sh_package_map_plan_payload_missing(const sh_package_map_plan *plan,
    sh_package_missing *out, char *error, size_t capacity)
{
    int ok = 0;
    AcquireSRWLockShared(&g_lock);
    if (plan && g_library_provider)
        ok = sh_package_compilation_payload_missing(plan->provider->compiled,
            pr_library_available, g_library_provider, out, error, capacity);
    else {
        sh_package_missing_free(out);
        if (error && capacity) snprintf(error, capacity, "payload availability requires a prepared map and local library");
    }
    ReleaseSRWLockShared(&g_lock); return ok;
}

int sh_package_map_plan_source_resources(const sh_package_map_plan *plan,
    sh_decl_registry_source registry, uintptr_t reflection,
    const char *json, size_t length, sh_package_references *out,
    sh_package_source_graph_report *report, char *error, size_t capacity)
{
    pr_original_source original = {sh_resource_catalog_read_path, plan ? plan->provider->catalog : NULL,
        plan ? plan->provider->native_assets : NULL};
    int ok = 0;
    if (error && capacity) error[0] = 0;
    if (out) sh_package_references_free(out);
    if (report) memset(report, 0, sizeof(*report));
#ifdef SH_PACKAGE_RUNTIME_TESTING
    if (g_test_empty_catalog) { original.game = g_test_baseline; original.context = g_test_baseline_context; }
#endif
    if (plan) {
        sh_package_source_audio audio = {plan->provider->audio_originals};
        sh_package_library_native native = sh_package_native_assets_adapter(plan->provider->native_assets);
        sh_package_source_admission admission = {native.source.context, native.source.admit};
        char scope_error[512];
        sh_package_native_assets_shader_scope_begin(plan->provider->native_assets);
        ok = sh_package_source_graph_with_contracts(plan->provider->compiled, plan->provider->catalog,
            pr_original, &original, registry, reflection, native.source.shader,
            audio.originals ? &audio : NULL, &admission, json, length, out, report, error, capacity);
        if (!sh_package_native_assets_shader_scope_end(plan->provider->native_assets, scope_error,
            sizeof(scope_error)) && ok) {
            ok = 0; if (out) sh_package_references_free(out);
            if (error && capacity) snprintf(error, capacity, "%s", scope_error);
        }
    }
    else if (error && capacity) snprintf(error, capacity, "candidate source graph requires a prepared map");
    return ok;
}

int sh_package_map_plan_usage_missing(const sh_package_map_plan *plan,
    sh_decl_registry_source registry, uintptr_t reflection,
    const char *json, size_t length, sh_package_missing *out,
    sh_package_source_graph_report *report, char *error, size_t capacity)
{
    sh_package_references required = {0};
    sh_package_source_graph_report inspected = {0};
    sh_package_file_identity map_identity = {0};
    const char **paths = NULL;
    char fallback[2048];
    int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (report) memset(report, 0, sizeof(*report));
    if (!out) { snprintf(error, capacity, "map resource availability output is required"); return 0; }
    sh_package_missing_free(out);
    AcquireSRWLockExclusive(&g_refresh_lock);
    __try {
        if (plan) {
            AcquireSRWLockExclusive(&g_lock);
            InterlockedExchange(&plan->provider->requirements_known, 0);
            sh_package_references_free(&plan->provider->required_paths);
            ReleaseSRWLockExclusive(&g_lock);
        }
        /* Read authored source and original baselines only. Installed overrides
         * prove availability later; their values cannot repair a missing source. */
        if (!sh_package_map_plan_source_resources(plan, registry, reflection, json, length,
            &required, &inspected, error, capacity)) goto done;
        if (required.incomplete || inspected.gaps) {
            snprintf(error, capacity, "Map resource coverage is incomplete: %s",
                inspected.first_gap[0] ? inspected.first_gap : "a dependency reader is unavailable");
            goto done;
        }
        if (required.count) {
            if (required.count > SIZE_MAX / sizeof(*paths) ||
                !(paths = malloc(required.count * sizeof(*paths)))) {
                snprintf(error, capacity, "cannot allocate required map resource paths"); goto done;
            }
            for (size_t i = 0; i < required.count; i++) {
                if (*required.items[i].type) {
                    snprintf(error, capacity, "Map resource coverage is incomplete: unresolved native identity");
                    goto done;
                }
                paths[i] = required.items[i].name;
            }
        }
        if (!sh_package_bytes_identity(json, length, &map_identity)) {
            snprintf(error, capacity, "cannot retain inspected map use identity"); goto done;
        }
        ok = sh_package_map_plan_missing(plan, paths, required.count, out, error, capacity);
        if (ok) {
            AcquireSRWLockExclusive(&g_lock);
            plan->provider->required_paths = required; memset(&required, 0, sizeof(required));
            plan->provider->required_map = map_identity;
            InterlockedExchange(&plan->provider->requirements_known, 1);
            ReleaseSRWLockExclusive(&g_lock);
        }
done:
        ;
    } __finally {
        if (report) *report = inspected;
        free(paths); sh_package_references_free(&required);
        ReleaseSRWLockExclusive(&g_refresh_lock);
    }
    return ok;
}

int sh_package_runtime_activate_prepared_map(sh_package_map_plan *plan,
    sh_package_activation_guard guard, sh_package_activation_fn activate, void *context)
{ return pr_update(NULL, 1, NULL, plan, NULL, guard, activate, context); }

void sh_package_map_plan_free(sh_package_map_plan *plan)
{
    if (!plan) return;
    AcquireSRWLockExclusive(&g_refresh_lock);
    pr_release(plan->provider);
    pr_release(plan->library); pr_release(plan->library_base); pr_release(plan->authoring);
    pr_inventory_release(plan->inventory);
    ReleaseSRWLockExclusive(&g_refresh_lock); free(plan);
}

const sh_package_compilation *sh_package_runtime_library_acquire(void)
{
    AcquireSRWLockShared(&g_lock);
    return g_library_provider ? g_library_provider->compiled : NULL;
}

int sh_package_runtime_has_map_provider(void)
{
    int active;
    AcquireSRWLockShared(&g_lock); active = g_map_provider != NULL; ReleaseSRWLockShared(&g_lock);
    return active;
}

int sh_package_runtime_refresh_guarded(const char *data_root, sh_package_activation_guard guard)
{
    return sh_package_runtime_refresh_activated(data_root, guard, NULL, NULL);
}

int sh_package_runtime_refresh(const char *data_root)
{
    return sh_package_runtime_refresh_guarded(data_root, (sh_package_activation_guard){0});
}

int sh_package_runtime_ready(void)
{
    int ready;
    AcquireSRWLockShared(&g_lock); ready = g_ready; ReleaseSRWLockShared(&g_lock); return ready;
}

int sh_package_runtime_uninitialized(void)
{
    int initial;
    AcquireSRWLockShared(&g_lock);
    initial = !g_current_provider && !g_activating && !g_native_preparing && !g_ready && !g_error[0];
    ReleaseSRWLockShared(&g_lock); return initial;
}

int sh_package_runtime_admission_ready(void)
{
    int ready;
    AcquireSRWLockShared(&g_lock); ready = g_usable && !g_activating && !g_native_preparing;
    if (ready) ready = sh_virtual_material_runtime_status(NULL, 0);
    ReleaseSRWLockShared(&g_lock); return ready;
}

void sh_package_runtime_error(char *out, size_t capacity)
{
    if (!out || !capacity) return;
    AcquireSRWLockShared(&g_lock); snprintf(out, capacity, "%s", g_error);
    if (!out[0]) sh_virtual_material_runtime_status(out, capacity);
    ReleaseSRWLockShared(&g_lock);
}

const sh_package_compilation *sh_package_runtime_acquire(void)
{
    AcquireSRWLockShared(&g_lock); return g_compiled;
}

const sh_resource_catalog *sh_package_runtime_catalog(void) { return g_catalog; }
int sh_package_runtime_serves_native_identity(const char *type, const char *name)
{
    int found = 0;
    if (!type || !name) return 0;
    AcquireSRWLockShared(&g_lock);
    for (size_t i = 0; g_compiled && i < g_compiled->resource_count; i++)
        if (sh_package_resource_native_matches(g_compiled->resources + i, type, name)) {
            found = 1; break;
        }
    ReleaseSRWLockShared(&g_lock); return found;
}
const sh_package_provenance *sh_package_runtime_bindings(void)
{ return g_current_provider ? &g_current_provider->bindings : NULL; }

sh_package_bundle *sh_package_runtime_source_bundle(size_t owner, const unsigned char compiler_tree[32])
{
    const pr_provider *provider = g_current_provider;
    if (!provider || !compiler_tree || owner >= provider->bundle_count ||
        owner >= provider->sources->package_count || !provider->sources->fingerprints ||
        memcmp(compiler_tree, provider->sources->fingerprints[owner], 32)) return NULL;
    return sh_package_bundle_retain(provider->bundles[owner]);
}

int sh_package_runtime_legacy_read(void *context, const char *type,
    const char *name, const char *path, unsigned char **body, size_t *length,
    char *error, size_t capacity)
{
    int result;
    (void)context;
    AcquireSRWLockShared(&g_lock);
    result = sh_resource_catalog_legacy_read(g_catalog, type, name, path, body, length, error, capacity);
    ReleaseSRWLockShared(&g_lock); return result;
}

int sh_package_runtime_sound_event(const char *name, uint32_t *event)
{
    int found;
    if (!event) return 0;
    AcquireSRWLockShared(&g_lock);
    found = sh_package_sound_event(g_compiled, name, event);
    ReleaseSRWLockShared(&g_lock);
    return found;
}

int sh_package_runtime_audio_programs(sh_package_runtime_audio **rows, size_t *count,
    char *error, size_t capacity)
{
    sh_package_runtime_audio *out = NULL; size_t used = 0;
    if (error && capacity) *error = 0;
    if (!rows || !count) return 0;
    *rows = NULL; *count = 0;
    AcquireSRWLockShared(&g_lock);
    const pr_provider *current = g_current_provider;
    size_t total = current ? current->audio_source_count : 0;
    if (total <= SIZE_MAX / sizeof(*out)) out = calloc(total ? total : 1, sizeof(*out));
    if (out) for (size_t i = 0; i < total; i++) {
        pr_provider *source = current->audio_sources[i];
        out[used].owner = pr_retain(source); out[used].program = source->audio;
#ifndef SH_PACKAGE_RUNTIME_TESTING
        out[used].lease = source->audio_ids;
#endif
        used++;
    }
    ReleaseSRWLockShared(&g_lock);
    if (!out) {
        if (error && capacity) snprintf(error, capacity, "cannot retain active audio programs");
        return 0;
    }
    *rows = out; *count = used; return 1;
}
void sh_package_runtime_audio_release(void *owner) { pr_release(owner); }
int sh_package_runtime_audio_baseline(sh_audio_shared_policy *out, char *error, size_t capacity)
{
    int result;
    AcquireSRWLockShared(&g_lock);
    sh_audio_originals *originals = g_current_provider ? g_current_provider->audio_originals : NULL;
    result = originals ? sh_package_audio_baseline_shared(originals, out, error, capacity) : 0;
    ReleaseSRWLockShared(&g_lock);
    if (!originals && error && capacity) snprintf(error, capacity, "native audio originals are unavailable");
    return result;
}

int sh_package_runtime_audio_original(const char *path, sh_package_original_identity *out,
    char *error, size_t capacity)
{
    int result;
    AcquireSRWLockShared(&g_lock);
    result = sh_audio_originals_identity(g_current_provider ? g_current_provider->audio_originals : NULL,
        path, out, error, capacity);
    ReleaseSRWLockShared(&g_lock);
    return result;
}

int sh_package_runtime_audio_originals_ready(void)
{
    int result;
    AcquireSRWLockShared(&g_lock);
    result = sh_audio_originals_ready(g_current_provider ? g_current_provider->audio_originals : NULL);
    ReleaseSRWLockShared(&g_lock);
    return result;
}

int sh_package_runtime_audio_original_banks(const char *language, const char *bank_prefix,
    sh_audio_originals_inventory_visit visit,
    void *context, char *error, size_t capacity)
{
    const sh_audio_original_bank *banks = NULL; size_t count = 0; int result;
    if (!visit) { if (error && capacity) snprintf(error, capacity, "audio metadata visitor is required"); return -1; }
    AcquireSRWLockShared(&g_lock);
    sh_audio_originals *originals = g_current_provider ? g_current_provider->audio_originals : NULL;
    if (!sh_audio_originals_matches(originals, language, bank_prefix)) {
        if (error && capacity) snprintf(error, capacity, "native audio environment changed after source capture");
        result = -1;
    } else result = sh_audio_originals_banks(originals, &banks, &count, error, capacity);
    if (result == 1) result = visit(context, banks, count, error, capacity);
    ReleaseSRWLockShared(&g_lock); return result;
}

int sh_package_runtime_audio_original_read(const char *path, uint64_t offset, void *out,
    size_t span, uint64_t *length, char *error, size_t capacity)
{
    int result;
    AcquireSRWLockShared(&g_lock);
    result = sh_audio_originals_read(g_current_provider ? g_current_provider->audio_originals : NULL,
        path, offset, out, span, length, error, capacity);
    ReleaseSRWLockShared(&g_lock);
    return result;
}

int sh_package_runtime_audio_packaged_banks(sh_audio_originals_bank_visit visit, void *visitor,
    char *error, size_t capacity)
{
    int result;
    AcquireSRWLockShared(&g_lock);
    result = sh_audio_originals_packaged_banks(g_current_provider ?
        g_current_provider->audio_originals : NULL, visit, visitor, error, capacity);
    ReleaseSRWLockShared(&g_lock);
    return result;
}

char *sh_package_runtime_summary(void)
{
    size_t i, j, length = 0, capacity;
    char *text;
    AcquireSRWLockShared(&g_lock);
    const char *rejections = g_library_provider ? g_library_provider->rejections : NULL;
    const char *coverage = g_library_provider ? g_library_provider->coverage : NULL;
    const char *unserved = g_library_provider ? g_library_provider->unserved : NULL;
    capacity = 4096 + (g_sources ? g_sources->package_count : 0) * 1024 +
        (rejections ? strlen(rejections) : 0) + (coverage ? strlen(coverage) : 0) +
        (unserved ? strlen(unserved) : 0);
    text = (char *)calloc(capacity, 1);
    if (!text) goto done;
    length += (size_t)snprintf(text + length, capacity - length, "Installed library: %zu packages.\n",
        g_inventory ? g_inventory->sources->package_count : 0);
    if (rejections) length += (size_t)snprintf(text + length, capacity - length, "%s%s", rejections,
        "Skipped packages stay unchanged on disk. Packages made for earlier Snapmap+ releases can be "
        "converted: close DOOM and run snapmap-plus migrate-overrides.\n");
    if (unserved) length += (size_t)snprintf(text + length, capacity - length, "%s%s", unserved,
        "Packages not served yet stay installed and unchanged; they need a Snapmap+ update, not an edit.\n");
    if (coverage) length += (size_t)snprintf(text + length, capacity - length, "%s", coverage);
    if (g_authoring_error[0]) length += (size_t)snprintf(text + length, capacity - length,
        "Local authoring composition unavailable: %s\nIndependent map packages can still compile.\n", g_authoring_error);
    if (!g_ready) length += (size_t)snprintf(text + length, capacity - length,
        "Packages could not compile: %s\n%s", g_error[0] ? g_error : "compiler has not initialized",
        g_compiled ? "The last successful compilation remains active.\n" : "");
    if (!g_sources || !g_compiled || !g_sources->package_count) {
        snprintf(text + length, capacity - length, "No packages in the active compilation.\n"); goto done;
    }
    length += (size_t)snprintf(text + length, capacity - length,
        "%zu packages; %zu native resources; %zu identical duplicates; %zu composed declarations.\n",
        g_sources->package_count, g_compiled->resource_count, g_compiled->duplicate_count, g_compiled->composed_count);
    for (i = 0; i < g_sources->package_count; i++) {
        const sh_package_descriptor *descriptor = NULL;
        size_t files = 0, components = 0, gameplay = 0;
        uint64_t bytes = 0;
        for (j = 0; j < g_sources->component_count; j++) if (g_sources->components[j].owner == i) {
            components++;
            if (!g_sources->components[j].relative[0]) descriptor = &g_sources->components[j].descriptor;
        }
        for (j = 0; j < g_sources->file_count; j++) if (g_sources->files[j].owner == i && !g_sources->files[j].directory) {
            files++; bytes += g_sources->files[j].length;
        }
        for (j = 0; j < g_compiled->resource_count; j++)
            gameplay += sh_package_owners_contains(&g_compiled->resources[j].gameplay_owners, i);
        length += (size_t)snprintf(text + length, capacity - length,
            "%s [%s]\n  %zu files, %llu bytes, %zu components; %s\n  %s\n",
            descriptor ? descriptor->name : "Package", descriptor ? descriptor->id : "?",
            files, (unsigned long long)bytes, components, gameplay ? "contains gameplay resources" : "editor support",
            g_sources->packages[i].root);
    }
done:
    ReleaseSRWLockShared(&g_lock); return text;
}

void sh_package_runtime_release(void) { ReleaseSRWLockShared(&g_lock); }

int sh_package_runtime_read(const char *engine_path, unsigned char **body, size_t *length)
{
    const sh_compiled_resource *resource;
    char error[1024] = "";
    int result = 0;
    if (body) *body = NULL;
    if (length) *length = 0;
    if (!body || !length) return -1;
    AcquireSRWLockShared(&g_lock);
    resource = sh_package_compilation_find(g_compiled, engine_path);
    if (resource) {
        *body = sh_package_compilation_read(g_compiled, resource, (size_t)PTRDIFF_MAX,
                                            length, error, sizeof(error));
        result = *body ? 1 : -1;
        if (result > 0 && resource->generated) {
            sh_resource_graph_frame frame;
            size_t i;
            /* Keep producer edges even for a preview read outside a native
             * parse. The held snapshot owns every path until recording ends. */
            sh_resource_graph_begin(&frame, "", engine_path);
            for (i = 0; i < resource->generated_input_count; i++)
                sh_resource_graph_file(resource->generated_inputs[i]);
            sh_resource_graph_end(&frame, 1);
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (result < 0) backend_log(error[0] ? error : "compiled package resource read failed");
    return result;
}

static char *pr_policy_section(const sh_package_policy *policy, const char *section, size_t *length)
{
    const sh_json_object *object = NULL;
    if (length) *length = 0;
    if (policy && section && length) {
        if (!strcmp(section, "requirements")) object = &policy->requirements;
        if (!strcmp(section, "strings")) object = &policy->strings;
        if (!strcmp(section, "hud")) object = &policy->hud;
    }
    return object ? sh_json_serialize_object(object, 0, length) : NULL;
}

int sh_package_runtime_open_file(const char *engine_path, FILE **stream, uint64_t *length)
{
    const sh_compiled_resource *resource;
    char error[1024] = "";
    int result = 0;
    if (stream) *stream = NULL;
    if (length) *length = 0;
    if (!stream || !length) return -1;
    AcquireSRWLockShared(&g_lock);
    resource = sh_package_compilation_find(g_compiled, engine_path);
    if (resource && !resource->body) {
        result = -1;
        *stream = sh_package_file_open(resource->cache_file, length, error, sizeof(error));
        if (*stream) result = 1;
    }
    ReleaseSRWLockShared(&g_lock);
    if (result < 0) backend_log(error[0] ? error : "package resource cache is unavailable");
    return result;
}

char *sh_package_runtime_policy(const char *section, size_t *length)
{
    const sh_package_compilation *compiled = sh_package_runtime_acquire();
    char *out = pr_policy_section(compiled ? &compiled->policy : NULL, section, length);
    sh_package_runtime_release(); return out;
}

/* Selection does not manufacture a new closure proof from live observations.
 * It can only preserve a source inspection for the same map bytes when every
 * additional observed path is already covered. Changed edits need inspection. */
static int pr_selection_keeps_requirements(const pr_provider *map,
    const char *json, size_t length, const sh_package_references *references)
{
    sh_package_file_identity identity;
    if (!map->requirements_known || !json || length != map->required_map.length ||
        !sh_package_bytes_identity(json, length, &identity) ||
        memcmp(identity.digest, map->required_map.digest, sizeof(identity.digest)) ||
        (references && references->incomplete)) return 0;
    for (size_t i = 0; references && i < references->count; i++) {
        sh_package_references one = {&references->items[i], 1, 0}, observed = {0};
        const char *type = references->items[i].type;
        int covered = 1;
        if (!_strnicmp(type, "snapeditor", 10) || !_strnicmp(type, "snappropertyinspector", 21)) continue;
        if (!sh_package_map_resources(map->compiled, map->catalog, "{}", 2, &one, &observed) ||
            !observed.count) covered = 0;
        for (size_t j = 0; covered && j < observed.count; j++) {
            size_t found;
            for (found = 0; found < map->required_paths.count; found++)
                if (!strcmp(observed.items[j].name, map->required_paths.items[found].name)) break;
            if (found == map->required_paths.count) covered = 0;
        }
        sh_package_references_free(&observed);
        if (!covered) return 0;
    }
    return 1;
}

int sh_package_runtime_select_map_occurrences(const char *json, size_t length,
    const sh_package_references *references, const sh_package_occurrence_snapshot *document,
    char *error, size_t capacity)
{
    sh_package_policy selected = {0}, old_policy;
    sh_package_references copied_references = {0}, old_references;
    char *copy = NULL, *old_json;
    int ok = 0;
    size_t i;
    sh_package_owners owners = {0};
    sh_package_provenance promoted = {0};
    if (error && capacity) error[0] = 0;
    const sh_package_provenance *uses = sh_package_occurrence_provenance(document);
    if (uses && (!json || uses->semantics != SH_PACKAGE_PROVENANCE_USES ||
        !sh_package_provenance_select_uses(uses, NULL, NULL, &promoted))) return 0;
    if (json) {
        if (!length || length == SIZE_MAX || !(copy = (char *)malloc(length + 1))) {
            if (error && capacity) snprintf(error, capacity, "cannot retain map package context");
            sh_package_provenance_free(&promoted); return 0;
        }
        memcpy(copy, json, length); copy[length] = 0;
        if (references) {
            copied_references.incomplete = references->incomplete;
            for (i = 0; i < references->count; i++)
                if (!sh_package_references_add(&copied_references, references->items[i].type,
                                                references->items[i].name)) {
                    free(copy); sh_package_references_free(&copied_references);
                    if (error && capacity) snprintf(error, capacity, "cannot retain map resource references");
                    sh_package_provenance_free(&promoted); return 0;
                }
        }
    } else if (length || (references && references->count)) { sh_package_provenance_free(&promoted); return 0; }
    AcquireSRWLockExclusive(&g_lock);
    if (g_activating || g_native_preparing) {
        if (error && capacity) snprintf(error, capacity, "package activation is in progress; retry map preparation");
        goto done;
    }
    if (copy) {
        sh_package_sources empty_sources = {0};
        sh_package_compilation empty = {0};
        empty.sources = &empty_sources;
        if (!(g_current_provider ? pr_exact_policy(g_current_provider, uses ? &promoted : &g_promoted, copy, length,
                &copied_references, &selected, &owners, error, capacity) :
            sh_package_map_policy(&empty, g_catalog, copy, length,
                &copied_references, &selected, &owners, NULL, error, capacity))) goto done;
    }
    old_json = g_map_json; old_policy = g_map_policy;
    g_map_json = copy; g_map_length = length; g_map_policy = selected;
    if (uses || !json) {
        sh_package_provenance_free(&g_promoted); g_promoted = promoted; memset(&promoted, 0, sizeof(promoted));
        g_promotion_generation++;
        InterlockedIncrement(&g_lease_generation);
    }
    old_references = g_map_references; g_map_references = copied_references;
    copied_references = old_references;
    if (g_map_provider && !pr_selection_keeps_requirements(g_map_provider, copy, length, &g_map_references))
        InterlockedExchange(&g_map_provider->requirements_known, 0);
    copy = old_json; selected = old_policy; ok = 1;
done:
    ReleaseSRWLockExclusive(&g_lock);
    free(copy); sh_package_policy_free(&selected); sh_package_references_free(&copied_references);
    if (ok) {
        char line[160];
        snprintf(line, sizeof(line), "package policy: %s; %llu gameplay package(s)",
                 json ? "map selected" : "map cleared", (unsigned long long)sh_package_owners_count(&owners));
        backend_log(line);
    }
    sh_package_owners_free(&owners); sh_package_provenance_free(&promoted);
    return ok;
}
int sh_package_runtime_select_map(const char *json, size_t length,
    const sh_package_references *references, char *error, size_t capacity)
{ return sh_package_runtime_select_map_occurrences(json, length, references, NULL, error, capacity); }

char *sh_package_runtime_active_policy(const char *section, size_t *length)
{
    char *out;
    AcquireSRWLockShared(&g_lock);
    out = pr_policy_section(&g_map_policy, section, length);
    ReleaseSRWLockShared(&g_lock);
    return out;
}

static char *pr_decl_alias_path(const char *alias)
{
    char *path, *key;
    size_t n;
    if (!alias) return NULL;
    key = sh_package_engine_path(alias);
    if (!key || strncmp(key, "decltree/", 9)) { free(key); return NULL; }
    n = strlen(key);
    if (n > SIZE_MAX - 8u) { free(key); return NULL; }
    path = (char *)malloc(n + 8u);
    if (path) snprintf(path, n + 8u, "generated/decls/%s", key + 9);
    free(key);
    return path;
}

int sh_package_runtime_decl_alias_exists(const char *alias)
{
    const sh_compiled_resource *resource;
    char *path = pr_decl_alias_path(alias);
    int exists;
    if (!path) return 0;
    AcquireSRWLockShared(&g_lock);
    resource = g_compiled ? sh_package_compilation_find(g_compiled, path) : NULL;
    exists = resource && resource->type && resource->body;
    ReleaseSRWLockShared(&g_lock);
    free(path);
    return exists;
}

int sh_package_runtime_read_decl_alias(const char *alias, unsigned char **body, size_t *length)
{
    const sh_compiled_resource *resource;
    char *path;
    char error[1024] = "";
    int result = -1;
    if (body) *body = NULL;
    if (length) *length = 0;
    if (!body || !length || !(path = pr_decl_alias_path(alias))) return -1;
    AcquireSRWLockShared(&g_lock);
    resource = g_compiled ? sh_package_compilation_find(g_compiled, path) : NULL;
    if (resource && resource->type) {
        *body = sh_package_compilation_read(g_compiled, resource, (size_t)INT_MAX, length, error, sizeof(error));
        result = *body ? 1 : -1;
    }
    /* Removed SnapMap originals are explicit restoration entries in the
     * compilation. Historical aliases cannot admit another resource or revive
     * a failed first activation after its provider was rolled back to NULL. */
    ReleaseSRWLockShared(&g_lock);
    free(path);
    return result;
}

/* All callbacks below read retained data or native reflection. They must not
 * load declarations while the provider is acquired. Native construction stays
 * in the admitted activation route. */
/* Every code a provider's registration keys hold, from its key bindings. */
static void pr_key_taken(const pr_provider *provider, sh_package_key_set *taken)
{
    for (size_t i = 0; provider && i < provider->bindings.binding_count; i++) {
        const sh_package_provenance_binding *binding = provider->bindings.bindings + i;
        sh_package_key_set held;
        char *end = NULL;
        unsigned long value;
        if (strcmp(binding->family, SH_PACKAGE_KEY_FAMILY)) continue;
        value = strtoul(binding->name, &end, 10);
        if (!end || *end || value > 0xfffffffful) continue;
        sh_package_key_decode(NULL, (uint32_t)value, &held);
        for (size_t w = 0; w < sizeof(held.words) / sizeof(held.words[0]); w++) taken->words[w] |= held.words[w];
    }
}
/* The group codes a document capture may extend: a copy of the serving
 * generation's table (the map's while one serves, else the library's), the
 * other table beside it and the codes their keys hold. The copy is adopted
 * only if the capture defines new sets. */
typedef struct pr_key_context {
    pr_provider *owner;
    const sh_package_key_groups *other;
    sh_package_key_groups *scratch;
    sh_package_key_set taken;
} pr_key_context;
static void pr_key_context_open(pr_key_context *keys, pr_provider *owner)
{
    memset(keys, 0, sizeof(*keys));
    keys->owner = owner;
    keys->other = owner && owner != g_library_provider && g_library_provider ? g_library_provider->key_groups : NULL;
    keys->scratch = owner ? calloc(1, sizeof(*keys->scratch)) : NULL;
    if (keys->scratch && owner->key_groups) *keys->scratch = *owner->key_groups;
    pr_key_taken(g_library_provider, &keys->taken);
    pr_key_taken(owner, &keys->taken);
}
/* Adopt new group codes a capture defined, and publish them (exclusive). */
static void pr_key_context_close(pr_key_context *keys, int adopt)
{
    if (adopt && keys->scratch && keys->owner &&
        keys->scratch->count > (keys->owner->key_groups ? keys->owner->key_groups->count : 0)) {
        AcquireSRWLockExclusive(&g_lock);
        sh_package_key_groups *old = keys->owner->key_groups;
        keys->owner->key_groups = keys->scratch; keys->scratch = old;
        pr_publish_key_groups();
        ReleaseSRWLockExclusive(&g_lock);
    }
    free(keys->scratch); keys->scratch = NULL;
}

void sh_package_runtime_key_decode(uint32_t value, sh_package_key_set *keys, uint32_t *historical)
{
    AcquireSRWLockShared(&g_lock);
    pr_provider *owner = g_map_provider ? g_map_provider : g_library_provider;
    const sh_package_key_groups *groups = owner ? owner->key_groups : NULL;
    const sh_package_key_groups *other = owner && owner != g_library_provider && g_library_provider ?
        g_library_provider->key_groups : NULL;
    uint32_t pattern = (value & SH_PACKAGE_KEY_SINGLE) >> 19;
    if ((value & SH_PACKAGE_KEY_MARKER) && !(groups && groups->defined[pattern]) && other && other->defined[pattern])
        groups = other;
    sh_package_key_decode(groups, value, keys);
    if (historical) *historical = owner && owner->key_groups ? owner->key_groups->historical : 0;
    ReleaseSRWLockShared(&g_lock);
}

int sh_package_runtime_key_code(const sh_package_key_set *keys, uint32_t *code)
{
    pr_key_context context;
    int result = -1;
    if (code) *code = 0;
    /* An editor edit never waits for a refresh; capture holds this lock too. */
    if (!keys || !code || !TryAcquireSRWLockExclusive(&g_refresh_lock)) return -1;
    AcquireSRWLockShared(&g_lock);
    pr_key_context_open(&context, g_map_provider ? g_map_provider : g_library_provider);
    if (context.scratch) {
        *code = sh_package_key_groups_code(context.scratch, context.other, &context.taken, keys);
        result = *code != 0;
    }
    ReleaseSRWLockShared(&g_lock);
    pr_key_context_close(&context, result == 1);
    ReleaseSRWLockExclusive(&g_refresh_lock);
    return result;
}
static char *pr_capture_document(const pr_provider *provider,
    sh_decl_registry_source registry, uintptr_t reflection, int retain_native,
    const sh_package_provenance *saved, const sh_package_bundle_binding *additional,
    size_t additional_count, const char *json, size_t length, size_t *output_length,
    sh_package_occurrence_snapshot **ownership, sh_mpkg_used **deliveries,
    size_t *delivery_count, pr_key_context *keys, char *error, size_t capacity)
{
    sh_package_bundle_binding *rows = NULL;
    sh_package_source_reader *reader = NULL;
    sh_package_provenance uses = {0};
    sh_package_references references = {0}, resources = {0};
    sh_package_source_binding inspection = {0};
    sh_package_occurrence_snapshot *next = NULL;
    sh_mpkg_used *used = NULL;
    char *out = NULL; size_t out_length = 0, count = 0, row_count;
    if (output_length) *output_length = 0;
    if (!provider || !provider->sources || !provider->compiled || !json ||
        !output_length || !ownership || (additional_count && !additional) ||
        (!!deliveries != !!delivery_count) || provider->bundle_count > SIZE_MAX - additional_count)
        goto done;
    row_count = provider->bundle_count + additional_count;
    if (row_count > SIZE_MAX / sizeof(*rows)) goto done;
    rows = calloc(row_count ? row_count : 1u, sizeof(*rows)); if (!rows) goto done;
    for (size_t i = 0; i < provider->bundle_count; i++) {
        if (i >= provider->sources->package_count || !provider->bundles[i] ||
            !provider->sources->fingerprints || strlen(provider->sources->packages[i].name) >= sizeof(rows[i].source_name)) goto done;
        strcpy_s(rows[i].source_name, sizeof(rows[i].source_name), provider->sources->packages[i].name);
        memcpy(rows[i].compiler_tree, provider->sources->fingerprints[i], 32);
        rows[i].original = provider->bundles[i];
    }
    if (additional_count) memcpy(rows + provider->bundle_count, additional, additional_count * sizeof(*rows));
    pr_original_source originals = {provider->catalog ? sh_resource_catalog_read_path : NULL,
        provider->catalog, provider->native_assets};
#ifdef SH_PACKAGE_RUNTIME_TESTING
    sh_package_runtime_test_source_inputs(NULL, NULL, &originals.game, &originals.context);
#endif
    reader = sh_package_source_reader_open(provider->compiled, provider->catalog,
        pr_original, &originals, registry, reflection, error, capacity);
    if (!reader) goto done;
    if (keys) sh_package_source_reader_key_codes(reader, keys->scratch, keys->other, &keys->taken,
        sh_package_map_uses_saved_keys);
    sh_mpkg_use_requirements required = {provider->compiled, provider->catalog};
    out = sh_package_map_uses_select_retained(reader, json, length, saved, &provider->bindings,
        rows, row_count, sh_mpkg_new_use_required, &required,
        retain_native ? pr_retained_use : NULL, (void *)provider, &out_length,
        &uses, &references, &inspection, error, capacity);
    if (!out) goto done;
    next = sh_package_occurrence_prepare_document(&uses, rows, row_count, error, capacity);
    if (!next) goto done;
    if (!pr_capture_leases(provider, next, error, capacity)) goto done;
    if (deliveries) {
        count = sh_mpkg_used_packages_for_uses(provider->compiled, provider->catalog,
            out, out_length, &references, &uses, rows, row_count, &used, &resources, error, capacity);
        if (count == SIZE_MAX) goto done;
    }
    sh_package_occurrence_release(*ownership); *ownership = next; next = NULL;
    if (deliveries) { sh_mpkg_used_free(*deliveries); *deliveries = used; used = NULL; *delivery_count = count; }
    *output_length = out_length;
done:
    if (!output_length || !*output_length) {
        free(out); out = NULL;
        if (error && capacity && !*error) snprintf(error, capacity, "exact map occurrence ownership could not be prepared");
    }
    sh_package_occurrence_release(next); sh_mpkg_used_free(used);
    sh_package_provenance_free(&uses); sh_package_references_free(&references);
    sh_package_references_free(&resources);
    sh_package_source_reader_close(reader); free(rows); return out;
}

char *sh_package_runtime_capture_document(const sh_package_map_plan *plan,
    const sh_package_provenance *saved, const sh_package_bundle_binding *additional,
    size_t additional_count, const char *json, size_t length, size_t *output_length,
    sh_package_occurrence_snapshot **ownership, sh_mpkg_used **deliveries,
    size_t *delivery_count, char *error, size_t capacity)
{
    char *out = NULL;
    if (error && capacity) *error = 0;
    if (output_length) *output_length = 0;
    if (!g_activation_callback) AcquireSRWLockExclusive(&g_refresh_lock);
    AcquireSRWLockShared(&g_lock);
    pr_key_context keys;
    pr_key_context_open(&keys, plan ? plan->provider : g_map_provider ? g_map_provider : g_library_provider);
#ifndef SH_PACKAGE_RUNTIME_TESTING
    sh_decl_registry_source registry = {0};
    sh_decl_native_source metadata = {NULL, pr_native_read, 0, g_native_scripts};
    if (sh_decl_server_registry_source(&registry) && sh_decl_native_source_ready(&metadata,
        g_native_accessor, g_native_types, g_native_game_system) == 1)
        out = pr_capture_document(plan ? plan->provider : g_current_provider, registry, metadata.reflection, !plan,
            saved, additional, additional_count, json, length, output_length, ownership,
            deliveries, delivery_count, &keys, error, capacity);
#else
    sh_decl_registry_source registry = {0}; uintptr_t reflection = 0;
    if (sh_package_runtime_test_source_inputs(&registry, &reflection, NULL, NULL))
        out = pr_capture_document(plan ? plan->provider : g_current_provider, registry, reflection, !plan,
            saved, additional, additional_count, json, length, output_length, ownership,
            deliveries, delivery_count, &keys, error, capacity);
#endif
    ReleaseSRWLockShared(&g_lock);
    pr_key_context_close(&keys, out != NULL);
    if (!g_activation_callback) ReleaseSRWLockExclusive(&g_refresh_lock);
    if (!out && error && capacity && !*error) snprintf(error, capacity, "native map occurrence reader is unavailable");
    return out;
}

#ifdef SH_PACKAGE_RUNTIME_TESTING
void sh_package_runtime_test_key_tables(const sh_package_key_groups *library, const sh_package_key_groups *map)
{
    AcquireSRWLockExclusive(&g_lock);
    pr_provider *owners[2] = {g_library_provider, g_map_provider};
    const sh_package_key_groups *tables[2] = {library, map};
    for (int i = 0; i < 2; i++) if (owners[i]) {
        free(owners[i]->key_groups); owners[i]->key_groups = NULL;
        if (tables[i] && (owners[i]->key_groups = malloc(sizeof(*tables[i])))) *owners[i]->key_groups = *tables[i];
    }
    pr_publish_key_groups();
    ReleaseSRWLockExclusive(&g_lock);
}
int sh_package_runtime_test_key_groups_current(void)
{
    AcquireSRWLockShared(&g_lock);
    int current = g_test_key_groups[0] == g_library_provider &&
        g_test_key_groups[1] == (g_library_provider ? g_library_provider->key_groups : NULL) &&
        g_test_key_groups[2] == g_map_provider && g_test_key_groups[3] == (g_map_provider ? g_map_provider->key_groups : NULL);
    ReleaseSRWLockShared(&g_lock);
    return current;
}
void sh_package_runtime_test_dispose(void)
{
    pr_provider *current, *library, *map;
    pr_inventory *inventory;
    AcquireSRWLockExclusive(&g_refresh_lock);
    AcquireSRWLockExclusive(&g_lock);
    current=g_current_provider; library=g_library_provider; map=g_map_provider;
    inventory=g_inventory; g_inventory=NULL;
    sh_virtual_material_runtime_restore(NULL);
    pr_publish(NULL,NULL,NULL);
    g_ready=g_usable=g_activating=g_native_preparing=0;
    pr_release(g_occurrence_recovery_provider); g_occurrence_recovery_provider = NULL;
    g_occurrence_recovery_context = NULL;
    free(g_map_json);g_map_json=NULL;g_map_length=0;
    sh_package_references_free(&g_map_references);
    sh_package_policy_free(&g_map_policy);
    ReleaseSRWLockExclusive(&g_lock);
    pr_release(current);pr_release(library);pr_release(map);pr_inventory_release(inventory);
    sh_package_provenance_free(&g_promoted);
    pr_collect_leases();
    ReleaseSRWLockExclusive(&g_refresh_lock);
}
#endif
