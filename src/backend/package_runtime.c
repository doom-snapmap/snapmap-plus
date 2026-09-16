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
#include "resource_types.h"
#include "decl_graph_compose.h"
#include "decl_md6_compose.h"
#include "decl_block_compose.h"
#include "package_material.h"
#include "grid_room_asset.h"
#include "resource_graph.h"
#include "audio_originals.h"
#include "audio_files_native.h"

static SRWLOCK g_lock = SRWLOCK_INIT;
static SRWLOCK g_refresh_lock = SRWLOCK_INIT;
/* Ownership changes are serialized by g_refresh_lock. Published pointers and
 * aliases change together under g_lock. A view borrows its catalog through a
 * retained library parent; every compilation/inventory remains independently
 * owned. Readers hold g_lock, not an untracked provider pointer. */
typedef struct pr_provider {
    sh_package_sources *sources;
    sh_package_compilation *compiled;
    sh_resource_catalog *catalog;
    sh_audio_originals *audio_originals;
    struct pr_provider *parent;
    char *data_root;
    char *rejections;
    size_t references;
} pr_provider;
typedef struct pr_inventory_entry { char *path; size_t source; } pr_inventory_entry;
typedef struct pr_inventory {
    sh_package_sources *sources;
    pr_inventory_entry *entries;
    size_t count, references;
} pr_inventory;
struct sh_package_map_plan { pr_provider *provider; pr_inventory *inventory; };
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
static char g_error[2048];
static char g_authoring_error[2048];
static char *g_map_json;
static size_t g_map_length;
static sh_package_references g_map_references;
static sh_package_policy g_map_policy;
static uintptr_t g_native_accessor, g_native_types, g_native_game_system;
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
    if (provider) provider->references++;
    return provider;
}

static void pr_release(pr_provider *provider)
{
    if (!provider || --provider->references) return;
    sh_package_compilation_free(provider->compiled);
    sh_package_sources_free(provider->sources);
    if (!provider->parent) {
        sh_resource_catalog_close(provider->catalog);
        sh_audio_originals_close(provider->audio_originals);
    }
    pr_release(provider->parent); free(provider->data_root); free(provider->rejections); free(provider);
}

static void pr_publish(pr_provider *current, pr_provider *library, pr_provider *map)
{
    g_current_provider = current; g_library_provider = library; g_map_provider = map;
    g_sources = current ? current->sources : NULL;
    g_compiled = current ? current->compiled : NULL;
    g_catalog = current ? current->catalog : NULL;
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
    sh_decl_native_source source = {NULL, pr_native_read, 0};
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
    sh_decl_native_source source = {NULL, pr_native_read, 0};
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
} pr_original_source;

static int pr_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    pr_original_source *source = (pr_original_source *)context;
    size_t i;
    int scope;
    *body = NULL; *length = 0;
    scope = source->game ? source->game(source->context, path, body, length) : 0;
    if (scope) return scope; /* An unreadable game original remains an error. */
    free(*body); *body = NULL; *length = 0;
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
    static LONG serial;
    size_t i, j;
    for (i = 0; i < compiled->resource_count; i++) {
        sh_compiled_resource *resource = &compiled->resources[i];
        const sh_package_source_file *source;
        sh_package_source_file cached;
        char digest[65], path[4352], staging[4416];
        wchar_t *wide = NULL, *temporary = NULL;
        DWORD attributes;
        int ok = 0;
        if (resource->body) continue; /* Compiled text already owns its bytes. */
        if (resource->cache_file) continue; /* A provider view retains its sealed bytes. */
        source = &compiled->sources->files[resource->source];
        cached = *source;
        if (!directory[0] && !pr_cache_directory(root, directory, sizeof(directory))) {
            snprintf(error, capacity, "cannot prepare package resource cache"); return 0;
        }
        for (j = 0; j < 32; j++) snprintf(digest + j * 2, 3, "%02x", source->digest[j]);
        snprintf(path, sizeof(path), "%s/%s", directory, digest);
        cached.absolute = path;
        wide = sh_package_source_wide_path(path);
        if (!wide) goto failed;
        attributes = GetFileAttributesW(wide);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) goto failed;
            resource->cache_file = sh_package_file_seal(&cached, error, capacity);
            ok = resource->cache_file != NULL;
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND) goto failed;
        if (!ok) {
            snprintf(staging, sizeof(staging), "%s.%lu.%llu.%lu.tmp", path, GetCurrentProcessId(),
                     (unsigned long long)GetTickCount64(),
                     (unsigned long)InterlockedIncrement(&serial));
            temporary = sh_package_source_wide_path(staging);
            if (temporary && sh_package_source_copy(source, staging)) {
                ok = MoveFileExW(temporary, wide, MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING) != 0;
                if (!ok) {
                    /* Another process may have published the same content. */
                    ok = sh_package_source_verify(&cached);
                    DeleteFileW(temporary);
                }
            }
        }
        if (ok) resource->cache_path = _strdup(path);
        if (ok && !resource->cache_file)
            resource->cache_file = sh_package_file_seal(&cached, error, capacity);
failed:
        free(wide); free(temporary);
        if (!ok || !resource->cache_path || !resource->cache_file) {
            snprintf(error, capacity, "cannot snapshot package resource: %s", resource->engine_path); return 0;
        }
    }
    return 1;
}

#ifdef SH_PACKAGE_RUNTIME_TESTING
/* Explicit empty-original catalog for filesystem/compiler integration tests.
 * Production always reads the installed game's catalog. */
static int g_test_empty_catalog;
static sh_package_baseline_reader g_test_baseline;
static void *g_test_baseline_context;
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
#endif

static int pr_activate(sh_package_activation_fn activate, void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    __try { return activate(context, restoring, changes, error, capacity) == 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(error, capacity, "native package %s raised an exception (0x%08lx)",
            restoring ? "recovery" : "activation", (unsigned long)GetExceptionCode());
        return 0;
    }
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

static pr_provider *pr_prepare(const char *data_root, const char *source_root,
    const pr_provider *previous, int product_only, pr_inventory **inventory_out,
    int *composition_failed, char *out_error, size_t out_capacity)
{
    sh_package_sources *sources = NULL;
    sh_package_compilation *compiled = NULL;
    sh_resource_catalog *catalog = NULL;
    sh_audio_originals *audio_originals = NULL;
    sh_package_baseline_reader baseline = NULL;
    void *baseline_context = NULL;
    sh_package_builtin builtins[sizeof(g_ov_baked_decls) / sizeof(g_ov_baked_decls[0])];
    sh_package_compile_environment environment = {0};
    sh_decl_native_schema *native_schema = NULL;
    char doom_base[4096], error[2048] = "";
    DWORD n;
    size_t default_index;
    char *slash, *rejections = NULL;
    size_t invalid_package = SIZE_MAX;
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
        if (strcat_s(doom_base, sizeof(doom_base), "\\base")) goto done;
        catalog = sh_resource_catalog_open(doom_base, error, sizeof(error));
        if (!catalog) goto done;
    }
    if (catalog) { baseline = sh_resource_catalog_read_path; baseline_context = catalog; }
#ifndef SH_PACKAGE_RUNTIME_TESTING
    if (catalog) {
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
    if (!source_root && !product_only) environment.invalid_package = &invalid_package;
    for (;;) {
        compiled = sh_package_compile_with(sources, &environment, error, sizeof(error));
        if (compiled || invalid_package == SIZE_MAX) break;
        if (invalid_package >= sources->package_count ||
            !pr_rejected(&rejections, &sources->packages[invalid_package], error) ||
            !sh_package_sources_remove(sources, invalid_package)) goto done;
    }
    if (inventory_out) {
        /* The inventory has its own diagnostic: a composition failure above
         * remains the reported cause when the inventory itself succeeds. */
        char inventory_error[2048] = "";
        sh_package_sources empty = {0};
        sh_package_sources *snapshot = sh_package_sources_join(sources, &empty, inventory_error, sizeof(inventory_error));
        if (snapshot) *inventory_out = pr_inventory_build(snapshot, inventory_error, sizeof(inventory_error));
        if (!*inventory_out) {
            strcpy_s(error, sizeof(error), inventory_error[0] ? inventory_error : "cannot retain the installed package inventory");
            goto done;
        }
    }
    if (!compiled) { if (composition_failed) *composition_failed = 1; goto done; }
    if (!pr_cache_resources(compiled, data_root, error, sizeof(error))) goto done;
    provider = calloc(1, sizeof(*provider));
    if (!provider) goto done;
    /* Remember where this compilation was scanned from so the library can be
     * recompiled later without the caller supplying the root again. */
    if (data_root && !source_root && !(provider->data_root = _strdup(data_root))) {
        free(provider); provider = NULL; goto done;
    }
    provider->sources = sources; provider->compiled = compiled; provider->catalog = catalog;
    provider->audio_originals = audio_originals; audio_originals = NULL;
    provider->references = 1;
    provider->rejections = rejections; rejections = NULL;
    sources = NULL; compiled = NULL; catalog = NULL;
done:
    if (!provider) snprintf(out_error, out_capacity, "%s", error[0] ? error : "package compilation could not be prepared");
    sh_package_compilation_free(compiled); sh_package_sources_free(sources); sh_resource_catalog_close(catalog);
    sh_audio_originals_close(audio_originals);
    sh_decl_native_schema_close(native_schema);
    free(rejections);
    return provider;
}

static pr_provider *pr_view(pr_provider *library, const pr_provider *map, char *error, size_t capacity)
{
    sh_package_sources empty_sources = {0};
    sh_package_compilation empty = {0};
    pr_provider *view = calloc(1, sizeof(*view));
    if (!view) return NULL;
    view->references = 1;
    empty.sources = &empty_sources; empty.canonical_path = library->compiled->canonical_path;
    view->compiled = sh_package_compilation_overlay(library->compiled, map ? map->compiled : &empty, error, capacity);
    if (!view->compiled) { pr_release(view); return NULL; }
    /* The view provider takes the independent inventory from the compiler. */
    view->sources = (sh_package_sources *)view->compiled->sources;
    view->compiled->owns_sources = 0;
    if (!map) { view->compiled->map_overlay = 0; view->compiled->map_owner_begin = 0; }
    view->parent = pr_retain(library); view->catalog = library->catalog;
    view->audio_originals = library->audio_originals;
    return view;
}

static int pr_compiled_available(void *context, const char *path, char *error, size_t capacity)
{ return sh_package_compilation_probe(context, path, error, capacity); }

static int pr_update(const char *data_root, int change_map, const char *map_source_root,
    const sh_package_map_plan *prepared,
    sh_package_activation_guard guard, sh_package_activation_fn activate, void *context)
{
    pr_provider *library = NULL, *map = NULL, *current = NULL;
    pr_provider *old_library, *old_map, *old_current;
    pr_inventory *inventory = NULL, *old_inventory = NULL;
    sh_package_policy selected = {0}, old_selected;
    sh_package_changes changes = {0};
    char *retired_map_json = NULL;
    size_t retired_map_length = 0;
    sh_package_references retired_references = {0};
    pr_original_source originals = {0};
    char error[2048] = "", line[512];
    char source_error[2048] = "";
    int ok = 0, guarded = 0, composition_failed = 0, old_usable = 0, library_rescanned = 0;
    AcquireSRWLockExclusive(&g_refresh_lock);
    if ((guard.begin != NULL) != (guard.end != NULL)) {
        snprintf(error, sizeof(error), "package activation guard is incomplete"); goto done;
    }
    if (change_map) {
        if (!g_library_provider || (map_source_root && !*map_source_root)) {
            snprintf(error, sizeof(error), "map resources require an initialized local library and a valid source root"); goto done;
        }
        library = pr_retain(g_library_provider);
        /* A committed installation belongs to the persistent library, not to the
         * temporary map provider that carried it through activation. Recompile the
         * library in this same transaction, before the overlay is composed or
         * retired, so those resources are never mistaken for removals and retired
         * out from under the consumers still using them. A failed rescan leaves
         * the current provider in place: nothing is retired on a guess. */
        if (InterlockedCompareExchange(&g_sources_changed, 0, 0) && library->data_root) {
            pr_provider *rescanned = pr_prepare(library->data_root, NULL, library, 0,
                &inventory, NULL, error, sizeof(error));
            if (!rescanned) {
                backend_log("package library rescan for committed sources failed; the current provider is kept");
                goto done;
            }
            /* A malformed newly installed unit may be isolated locally, but
             * cannot retire the map provider which still supplies its bytes. */
            sh_package_missing missing = {0};
            int complete = !g_map_provider || sh_package_compilation_payload_missing(
                g_map_provider->compiled, pr_compiled_available, rescanned->compiled,
                &missing, error, sizeof(error));
            if (!complete || missing.count) {
                if (missing.count) snprintf(error, sizeof(error), "installed packages no longer supply the active map; repair the rejected package before leaving its provider");
                sh_package_missing_free(&missing); pr_release(rescanned); goto done;
            }
            sh_package_missing_free(&missing);
            pr_release(library); library = rescanned; library_rescanned = 1;
        }
        if (prepared) map = pr_retain(prepared->provider);
        else if (map_source_root && !(map = pr_prepare(data_root, map_source_root,
            g_map_provider, 0, NULL, NULL, error, sizeof(error)))) goto done;
    } else {
        library_rescanned = 1;
        library = pr_prepare(data_root, NULL, g_library_provider, 0, &inventory,
            &composition_failed, error, sizeof(error));
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
            library = pr_prepare(data_root, NULL, NULL, 1, NULL, NULL, error, sizeof(error));
            if (!library) goto done;
        }
        map = pr_retain(g_map_provider);
    }
    current = pr_view(library, map, error, sizeof(error));
    if (!current) goto done;
    if (library->catalog) { originals.game = sh_resource_catalog_read_path; originals.context = library->catalog; }
#ifdef SH_PACKAGE_RUNTIME_TESTING
    if (g_test_empty_catalog) { originals.game = g_test_baseline; originals.context = g_test_baseline_context; }
#endif
    if (!sh_package_compilation_restore_missing(current->compiled, g_compiled,
        pr_original, &originals, error, sizeof(error))) goto done;
    if (activate && !sh_package_compilation_changes(g_compiled, current->compiled,
        &changes, error, sizeof(error))) goto done;
    if (guard.begin) { guard.begin(); guarded = 1; }
    AcquireSRWLockExclusive(&g_lock);
    if (!(change_map && !map) && g_map_json &&
        !sh_package_map_policy(current->compiled, current->catalog, g_map_json, g_map_length,
        &g_map_references, &selected, NULL, NULL, error, sizeof(error))) {
        ReleaseSRWLockExclusive(&g_lock); goto done;
    }
    old_current = g_current_provider; old_library = g_library_provider; old_map = g_map_provider;
    old_usable = g_usable;
    if (inventory) { old_inventory = g_inventory; g_inventory = inventory; }
    old_selected = g_map_policy; g_map_policy = selected; memset(&selected, 0, sizeof(selected));
    if (change_map && !map) {
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
        selected = g_map_policy; g_map_policy = old_selected;
        if (change_map && !map) {
            g_map_json = retired_map_json; g_map_length = retired_map_length;
            g_map_references = retired_references;
            retired_map_json = NULL; memset(&retired_references, 0, sizeof(retired_references));
        }
        pr_publish(old_current, old_library, old_map);
        if (inventory) g_inventory = old_inventory;
        ReleaseSRWLockExclusive(&g_lock);
        if (guarded) { guard.end(1); guarded = 0; }
        for (size_t i = 0; i < changes.count; i++) {
            if (changes.items[i].kind == SH_PACKAGE_RESOURCE_ADDED)
                changes.items[i].kind = SH_PACKAGE_RESOURCE_REMOVED;
            else if (changes.items[i].kind == SH_PACKAGE_RESOURCE_REMOVED)
                changes.items[i].kind = SH_PACKAGE_RESOURCE_ADDED;
        }
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
    ReleaseSRWLockExclusive(&g_lock);
    snprintf(line, sizeof(line), "package compiler: %zu packages, %zu components, %zu native resources; %zu identical duplicates, %zu composed declarations%s",
        current->sources->package_count, current->sources->component_count, current->compiled->resource_count,
        current->compiled->duplicate_count, current->compiled->composed_count, map ? "; temporary map provider active" : "");
    backend_log(line);
    if (library_rescanned) InterlockedExchange(&g_sources_changed, 0);
    library = map = current = NULL;
    if (inventory) { pr_inventory_release(old_inventory); inventory = NULL; }
    pr_release(old_current); pr_release(old_library); pr_release(old_map);
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
    ReleaseSRWLockExclusive(&g_refresh_lock); return ok;
}

int sh_package_runtime_refresh_activated(const char *data_root, sh_package_activation_guard guard,
    sh_package_activation_fn activate, void *context)
{
    return pr_update(data_root, 0, NULL, NULL, guard, activate, context);
}

int sh_package_runtime_activate_map(const char *data_root, const char *map_source_root,
    sh_package_activation_guard guard, sh_package_activation_fn activate, void *context)
{
    return pr_update(data_root, 1, map_source_root, NULL, guard, activate, context);
}

sh_package_map_plan *sh_package_runtime_prepare_map(const char *data_root,
    const char *map_source_root, char *error, size_t capacity)
{
    sh_package_map_plan *plan = NULL;
    char fallback[2048];
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    AcquireSRWLockExclusive(&g_refresh_lock);
    if (!data_root || !*data_root || !map_source_root || !*map_source_root || !g_library_provider) {
        snprintf(error, capacity, "map preparation requires a local library, data root and private source root");
        goto done;
    }
    plan = calloc(1, sizeof(*plan));
    if (!plan) { snprintf(error, capacity, "cannot allocate map preparation"); goto done; }
    plan->provider = pr_prepare(data_root, map_source_root, g_map_provider, 0, NULL, NULL, error, capacity);
    if (!plan->provider) { free(plan); plan = NULL; }
done:
    ReleaseSRWLockExclusive(&g_refresh_lock); return plan;
}

const sh_package_compilation *sh_package_map_plan_compilation(const sh_package_map_plan *plan)
{ return plan ? plan->provider->compiled : NULL; }

int sh_package_map_plan_prepare_inventory(sh_package_map_plan *plan,
    const char *data_root, char *error, size_t capacity)
{
    pr_inventory *inventory;
    if (!plan || !data_root || !*data_root) {
        if (error && capacity) snprintf(error, capacity, "installed inventory requires a prepared map and data root");
        return 0;
    }
    AcquireSRWLockExclusive(&g_refresh_lock);
    int composition_failed = 0;
    pr_provider *checked = pr_prepare(data_root, NULL, g_library_provider, 0,
        &inventory, &composition_failed, error, capacity);
    pr_release(checked);
    if (!inventory || (!checked && !composition_failed)) {
        pr_inventory_release(inventory);
        ReleaseSRWLockExclusive(&g_refresh_lock); return 0;
    }
    pr_inventory_release(plan->inventory); plan->inventory = inventory;
    ReleaseSRWLockExclusive(&g_refresh_lock); return 1;
}

int sh_package_runtime_commit_map(sh_package_map_plan *plan,
    int (*commit)(char *error, size_t capacity), char *error, size_t capacity)
{
    pr_inventory *previous = NULL;
    char fallback[512];
    int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    AcquireSRWLockExclusive(&g_refresh_lock);
    if (!commit || g_activating || !g_usable ||
        g_map_provider != (plan ? plan->provider : NULL)) {
        snprintf(error, capacity, "the prepared map is no longer active"); goto done;
    }
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
        previous = g_inventory; g_inventory = pr_inventory_retain(plan->inventory);
        ReleaseSRWLockExclusive(&g_lock);
    }
done:
    pr_inventory_release(previous);
    ReleaseSRWLockExclusive(&g_refresh_lock); return ok;
}

static int pr_library_available(void *context, const char *path, char *error, size_t capacity)
{
    const pr_provider *library = context;
    unsigned char *original = NULL;
    size_t length = 0;
    int present, invalid = 0;
    char *canonical = pr_canonical(path);
    pr_original_source source = {sh_resource_catalog_read_path, library->catalog};
    if (!canonical) {
        if (error && capacity) snprintf(error, capacity, "invalid installed resource path: %s", path);
        return -1;
    }
    size_t first = 0, last = g_inventory ? g_inventory->count : 0;
    while (first < last) {
        size_t middle = first + (last - first) / 2;
        if (strcmp(g_inventory->entries[middle].path, canonical) < 0) first = middle + 1;
        else last = middle;
    }
    for (size_t i = first; g_inventory && i < g_inventory->count &&
        !strcmp(g_inventory->entries[i].path, canonical); i++) {
        const sh_package_source_file *file = &g_inventory->sources->files[g_inventory->entries[i].source];
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
    sh_package_compilation *candidate = NULL;
    pr_original_source original = {sh_resource_catalog_read_path, plan ? plan->provider->catalog : NULL};
    int ok = 0;
    if (error && capacity) error[0] = 0;
    if (out) sh_package_references_free(out);
    if (report) memset(report, 0, sizeof(*report));
    AcquireSRWLockShared(&g_lock);
    if (plan && g_library_provider)
        candidate = sh_package_compilation_overlay(g_library_provider->compiled,
            plan->provider->compiled, error, capacity);
    ReleaseSRWLockShared(&g_lock);
#ifdef SH_PACKAGE_RUNTIME_TESTING
    if (g_test_empty_catalog) { original.game = g_test_baseline; original.context = g_test_baseline_context; }
#endif
    if (candidate) ok = sh_package_source_graph(candidate, plan->provider->catalog,
        pr_original, &original, registry, reflection, json, length, out, report, error, capacity);
    else if (error && capacity && !error[0]) snprintf(error, capacity, "candidate source graph requires a map and retained library");
    sh_package_compilation_free(candidate); return ok;
}

int sh_package_runtime_activate_prepared_map(sh_package_map_plan *plan,
    sh_package_activation_guard guard, sh_package_activation_fn activate, void *context)
{ return pr_update(NULL, 1, NULL, plan, guard, activate, context); }

void sh_package_map_plan_free(sh_package_map_plan *plan)
{
    if (!plan) return;
    AcquireSRWLockExclusive(&g_refresh_lock);
    pr_release(plan->provider);
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

int sh_package_runtime_admission_ready(void)
{
    int ready;
    AcquireSRWLockShared(&g_lock); ready = g_usable && !g_activating;
    ReleaseSRWLockShared(&g_lock); return ready;
}

void sh_package_runtime_error(char *out, size_t capacity)
{
    if (!out || !capacity) return;
    AcquireSRWLockShared(&g_lock); snprintf(out, capacity, "%s", g_error); ReleaseSRWLockShared(&g_lock);
}

const sh_package_compilation *sh_package_runtime_acquire(void)
{
    AcquireSRWLockShared(&g_lock); return g_compiled;
}

const sh_resource_catalog *sh_package_runtime_catalog(void) { return g_catalog; }

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
    capacity = 4096 + (g_sources ? g_sources->package_count : 0) * 1024 +
        (rejections ? strlen(rejections) : 0);
    text = (char *)calloc(capacity, 1);
    if (!text) goto done;
    length += (size_t)snprintf(text + length, capacity - length, "Installed library: %zu packages.\n",
        g_inventory ? g_inventory->sources->package_count : 0);
    if (rejections) length += (size_t)snprintf(text + length, capacity - length, "%s%s", rejections,
        "Skipped packages stay unchanged on disk. Packages made for earlier Snapmap+ releases can be "
        "converted: close DOOM and run snapmap-plus migrate-overrides.\n");
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

int sh_package_runtime_select_map(const char *json, size_t length,
    const sh_package_references *references, char *error, size_t capacity)
{
    sh_package_policy selected = {0}, old_policy;
    sh_package_references copied_references = {0}, old_references;
    char *copy = NULL, *old_json;
    int ok = 0;
    size_t i;
    sh_package_owners owners = {0};
    if (error && capacity) error[0] = 0;
    if (json) {
        if (!length || length == SIZE_MAX || !(copy = (char *)malloc(length + 1))) {
            if (error && capacity) snprintf(error, capacity, "cannot retain map package context");
            return 0;
        }
        memcpy(copy, json, length); copy[length] = 0;
        if (references) {
            copied_references.incomplete = references->incomplete;
            for (i = 0; i < references->count; i++)
                if (!sh_package_references_add(&copied_references, references->items[i].type,
                                                references->items[i].name)) {
                    free(copy); sh_package_references_free(&copied_references);
                    if (error && capacity) snprintf(error, capacity, "cannot retain map resource references");
                    return 0;
                }
        }
    } else if (length || (references && references->count)) return 0;
    AcquireSRWLockExclusive(&g_lock);
    if (g_activating) {
        if (error && capacity) snprintf(error, capacity, "package activation is in progress; retry map preparation");
        goto done;
    }
    if (copy) {
        sh_package_sources empty_sources = {0};
        sh_package_compilation empty = {0};
        empty.sources = &empty_sources;
        if (!sh_package_map_policy(g_compiled ? g_compiled : &empty, g_catalog,
                                    copy, length, &copied_references, &selected, &owners, NULL, error, capacity)) goto done;
    }
    old_json = g_map_json; old_policy = g_map_policy;
    g_map_json = copy; g_map_length = length; g_map_policy = selected;
    old_references = g_map_references; g_map_references = copied_references;
    copied_references = old_references;
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
    sh_package_owners_free(&owners);
    return ok;
}

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

#ifdef SH_PACKAGE_RUNTIME_TESTING
void sh_package_runtime_test_dispose(void)
{
    pr_provider *current, *library, *map;
    pr_inventory *inventory;
    AcquireSRWLockExclusive(&g_refresh_lock);
    AcquireSRWLockExclusive(&g_lock);
    current=g_current_provider; library=g_library_provider; map=g_map_provider;
    inventory=g_inventory; g_inventory=NULL;
    pr_publish(NULL,NULL,NULL);
    g_ready=g_usable=g_activating=0;
    free(g_map_json);g_map_json=NULL;g_map_length=0;
    sh_package_references_free(&g_map_references);
    sh_package_policy_free(&g_map_policy);
    ReleaseSRWLockExclusive(&g_lock);
    pr_release(current);pr_release(library);pr_release(map);pr_inventory_release(inventory);
    ReleaseSRWLockExclusive(&g_refresh_lock);
}
#endif
