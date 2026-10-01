#include <limits.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
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
#include "package_spawn_registry.h"
#include "package_material.h"
#include "grid_room_asset.h"
#include "resource_graph.h"
#include "audio_originals.h"
#include "audio_files_native.h"
#include "package_library.h"
#if !defined(SH_PACKAGE_RUNTIME_TESTING) || defined(SH_SPAWN_KEYS_TESTING)
#include "spawn_keys.h"
#define PR_NATIVE_SPAWN 1
#endif
#include "package_hud.h"
#include "package_sound.h"
#include "fsm_web.h"
#include "package_native_assets.h"
#include "package_map_uses.h"
#include "package_native_inline.h"
#include "package_editor_rows.h"
#include "map_embed.h"
#include "map_provenance.h"
#include "strids.h"
#ifndef SH_PACKAGE_RUNTIME_TESTING
#include "decl_server.h"
#include "resource_graph_native.h"
#include "engine_globals.h"
#include "host_image.h"
#include "package_sound_native.h"
#include "audio_banks_native.h"
#endif

static SRWLOCK g_lock = SRWLOCK_INIT;
static SRWLOCK g_refresh_lock = SRWLOCK_INIT;
/* Native private identities survive provider retirement and failed activation.
 * Access is serialized with all provider compilation by g_refresh_lock. */
static sh_package_link_history *g_link_history;
/* An installed package a library rejected on its own bytes alone, with the
 * exact source tree it read (see pr_exclusions_apply). */
typedef struct pr_exclusion {
    char *name;             /* sh_package.name */
    unsigned char tree[32]; /* its source fingerprint when judged */
    char *line;             /* the report line it produced */
} pr_exclusion;
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
    /* AI state-machine webs it binds and declarations it owns (package_fsm.h). */
    sh_package_fsm_webs *fsm_webs;
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
#ifdef PR_NATIVE_SPAWN
    sh_spawn_keys_program *spawn_program;
#endif
    uint32_t *unmodified_types;
    size_t unmodified_count;
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
    pr_exclusion *exclusions; /* Rejections decided on a package's own bytes alone. */
    size_t exclusion_count;
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
    char *family; /* Native selector roots, not every resource in the provider. */
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
/* Discovery recordings later compiles of this process replay after exact
 * validation (sh_package_source_memo). Compilations are serialized under
 * g_refresh_lock; a capture that finds it borrowed records for itself. */
static sh_package_source_memo *g_source_memo;
/* Set when a published installation that no map load continues (the MPKG
 * gate) requests the refresh that folds it into the library. */
static volatile LONG g_install_published;
void sh_package_runtime_note_install_published(void)
{
    InterlockedExchange(&g_install_published, 1);
}

static pr_provider *pr_retain(pr_provider *provider)
{
    if (provider) InterlockedIncrement(&provider->references);
    return provider;
}

static void pr_exclusions_free(pr_exclusion *items, size_t count)
{
    for (size_t i = 0; items && i < count; i++) { free(items[i].name); free(items[i].line); }
    free(items);
}
static int pr_exclusion_add(pr_exclusion **items, size_t *count, const char *name,
    const unsigned char tree[32], const char *line)
{
    pr_exclusion *grown = *count < SIZE_MAX / sizeof(**items) - 1 ?
        realloc(*items, (*count + 1) * sizeof(**items)) : NULL;
    if (!grown) return 0;
    *items = grown;
    pr_exclusion *item = grown + *count;
    item->name = _strdup(name); item->line = _strdup(line ? line : "");
    if (!item->name || !item->line) { free(item->name); free(item->line); return 0; }
    memcpy(item->tree, tree, 32); (*count)++;
    return 1;
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
    sh_package_fsm_webs_free(provider->fsm_webs);
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
    free(provider->unserved); free(provider->coverage); free(provider->key_groups); free(provider->unmodified_types);
    pr_exclusions_free(provider->exclusions, provider->exclusion_count);
#ifdef PR_NATIVE_SPAWN
    sh_spawn_keys_program_release(provider->spawn_program);
#endif
    free(provider);
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

/* A serving view owns the complete dictionary, including native consumers
 * whose source map has closed. Checking the union while preparing that view
 * keeps resource and numeric-key publication in one activation transaction. */
static int pr_key_groups_add(pr_provider *view, const pr_provider *source, char *error, size_t capacity)
{
    if (!source || !source->key_groups || view == source) return 1;
    if (!view->key_groups && !(view->key_groups = calloc(1, sizeof(*view->key_groups)))) {
        snprintf(error, capacity, "cannot retain registration key dictionary"); return 0;
    }
    if (!sh_package_key_groups_merge(view->key_groups, source->key_groups)) {
        snprintf(error, capacity, "registration key dictionary changed while its consumers are retained"); return 0;
    }
    return 1;
}
static int pr_spawn_classifications(pr_provider *view, char *error, size_t capacity);
static int pr_prepare_spawn_program(pr_provider *view, char *error, size_t capacity)
{
    if (!pr_spawn_classifications(view, error, capacity)) return 0;
#ifdef PR_NATIVE_SPAWN
    sh_spawn_keys_program *program = sh_spawn_keys_program_create(view->key_groups,
        view->unmodified_types, view->unmodified_count);
    if (!program) { snprintf(error, capacity, "cannot prepare native spawn dispatch metadata"); return 0; }
    sh_spawn_keys_program_release(view->spawn_program); view->spawn_program = program;
#else
    (void)view; (void)error; (void)capacity;
#endif
    return 1;
}
#ifdef SH_PACKAGE_RUNTIME_TESTING
static const void *g_test_key_groups[2];
#endif
static void pr_publish_key_groups(void)
{
#ifdef PR_NATIVE_SPAWN
    sh_spawn_keys_publish_program(g_current_provider ? g_current_provider->spawn_program : NULL);
#endif
#ifdef SH_PACKAGE_RUNTIME_TESTING
    g_test_key_groups[0] = g_current_provider;
    g_test_key_groups[1] = g_current_provider ? g_current_provider->key_groups : NULL;
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
/* One compile's shipped editor entries, read on the library's first request
 * (sh_package_library_native.editor_rows); state 0 unread, 1 read, 2 none. */
typedef struct pr_compile_rows {
    const sh_resource_catalog *catalog;
    const pr_original_source *originals;
    sh_editor_rows rows;
    int state;
} pr_compile_rows;
static int pr_compile_rows_read(void *context, const struct sh_editor_rows **rows);

/* The state-machine placeholder (fsm_web.h, package_fsm.h): a product
 * built-in derived from the installed stock declaration, so a provider that
 * lacks it restores it (sh_package_compilation_restore_missing). */
static int pr_fsm_placeholder_read(sh_package_baseline_reader game, void *context,
    unsigned char **body, size_t *length)
{
    unsigned char *stock = NULL;
    size_t stock_length = 0;
    char error[256];
    *body = NULL; *length = 0;
    int status = game ? game(context, "generated/decls/aifsmmanager/" SH_PACKAGE_FSM_PLACEHOLDER_SOURCE ".decl",
        &stock, &stock_length) : 0;
    if (status != 1) { free(stock); return status < 0 ? -1 : 0; }
    *body = (unsigned char *)sh_package_fsm_placeholder((sh_decl_source){(const char *)stock, stock_length},
        length, error, sizeof(error));
    free(stock);
    return *body ? 3 : -1;
}

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
    if (!_stricmp(path, "generated/decls/aifsmmanager/" SH_PACKAGE_FSM_PLACEHOLDER ".decl"))
        return pr_fsm_placeholder_read(source->game, source->context, body, length);
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

/* The same state path when package-cache already exists, creating nothing:
 * reading or removing state never creates a cache. Ancestors are checked like
 * pr_cache_directory (no redirected directory). 1 present, 0 absent/refused. */
static int pr_cache_state_existing(const char *root, const char *name, char *out, size_t capacity)
{
    char cache[4096];
    wchar_t *wide = sh_package_source_wide_path(root);
    int ok = 1;
    if (!wide) return 0;
    for (size_t i = 7; ; i++) if (!wide[i] || wide[i] == L'\\') {
        wchar_t saved = wide[i];
        DWORD attributes;
        wide[i] = 0; attributes = GetFileAttributesW(wide); wide[i] = saved;
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { ok = 0; break; }
        if (!saved) break;
    }
    free(wide);
    if (!ok || snprintf(cache, sizeof(cache), "%s/package-cache", root) >= (int)sizeof(cache)) return 0;
    wide = sh_package_source_wide_path(cache);
    DWORD attributes = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    free(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    int written = snprintf(out, capacity, "%s/%s", cache, name);
    return written > 0 && (size_t)written < capacity;
}
/* Small derived-state files (remembered decisions, preparation hints) live in
 * package-cache itself, beside the content-addressed resources directory. */
static int pr_cache_state(const char *root, const char *name, char *out, size_t capacity)
{
    char directory[4096];
    size_t n;
    if (!pr_cache_directory(root, directory, sizeof(directory)) || (n = strlen(directory)) < 10 ||
        strcmp(directory + n - 10, "/resources")) return 0;
    directory[n - 10] = 0;
    int written = snprintf(out, capacity, "%s/%s", directory, name);
    return written > 0 && (size_t)written < capacity;
}
/* Whole-file state I/O with wide paths: the data root may be any Unicode path. */
static char *pr_state_read(const char *path, size_t limit, size_t *length)
{
    wchar_t *wide = sh_package_source_wide_path(path);
    FILE *file = NULL;
    char *text = NULL;
    *length = 0;
    if (!wide) return NULL;
    if (!_wfopen_s(&file, wide, L"rb") && file) {
        size_t n = 0;
        text = malloc(limit);
        if (text) n = fread(text, 1, limit, file);
        if (!text || n >= limit || ferror(file)) { free(text); text = NULL; }
        else *length = n;
        fclose(file);
    }
    free(wide); return text;
}
static int pr_state_write(const char *path, const char *text)
{
    char temporary[4400];
    wchar_t *wide = NULL, *wide_temporary = NULL;
    FILE *file = NULL;
    int ok = 0;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) return 0;
    wide = sh_package_source_wide_path(path); wide_temporary = sh_package_source_wide_path(temporary);
    if (wide && wide_temporary && !_wfopen_s(&file, wide_temporary, L"wb") && file) {
        size_t size = strlen(text);
        ok = fwrite(text, 1, size, file) == size;
        ok = fclose(file) == 0 && ok;
        if (!ok || !MoveFileExW(wide_temporary, wide, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(wide_temporary); ok = 0; }
    }
    free(wide); free(wide_temporary); return ok;
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
static int g_test_spawn_keys;
void sh_package_runtime_test_installed_links(int enabled) { g_test_installed_links = enabled; }
void sh_package_runtime_test_spawn_keys(int enabled) { g_test_spawn_keys = enabled; }
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
static const char *const *g_test_editor_rows;
static size_t g_test_editor_row_count;
static int g_test_editor_rows_unavailable;
void sh_package_runtime_test_editor_rows(const char *const *names, size_t count)
{ g_test_editor_rows = names; g_test_editor_row_count = count; }
void sh_package_runtime_test_editor_rows_unavailable(int unavailable) { g_test_editor_rows_unavailable = unavailable; }
static sh_package_string_reservations g_test_strings;
void sh_package_runtime_test_strings(const sh_package_string_reservations *strings)
{ if (strings) g_test_strings = *strings; else memset(&g_test_strings, 0, sizeof(g_test_strings)); }
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

/* AI state-machine webs (fsm_web.h). Without the hook the engine would merge
 * every served private state machine into each web its layers meet, so a view
 * with any is refused. Its product declarations are issued before they are
 * registered, and its table is published once they are; a refused table is an
 * activation failure. Every recovery publishes the restored view's table again
 * (issued names stay excluded), and only a recovery that did so is usable. */
typedef struct pr_fsm_hook {
    int (*installed)(void);
    int (*issue)(const char *const *names, size_t count);
    int (*publish)(const sh_fsm_web_table *table);
} pr_fsm_hook;
#ifndef SH_PACKAGE_RUNTIME_TESTING
static const pr_fsm_hook g_fsm = {sh_fsm_web_installed, sh_fsm_web_issue, sh_fsm_web_publish};
#else
static pr_fsm_hook g_fsm; /* None: a compiler fixture without the native hook. */
void sh_package_runtime_test_fsm_hook(int (*installed)(void),
    int (*issue)(const char *const *names, size_t count), int (*publish)(const sh_fsm_web_table *table))
{ g_fsm = (pr_fsm_hook){installed, issue, publish}; }
#endif
static int pr_fsm_content(const pr_provider *view)
{ return view && view->fsm_webs && (view->fsm_webs->count || view->fsm_webs->owned_count); }
static int pr_fsm_issue(const pr_provider *view, char *error, size_t capacity)
{
    const sh_package_fsm_webs *webs = view ? view->fsm_webs : NULL;
    if (!pr_fsm_content(view) || !g_fsm.installed) return 1;
    if (!g_fsm.installed()) {
        snprintf(error, capacity, "package AI state machines need the state-machine web hook, which is not installed for this game build");
        return 0;
    }
    if (!g_fsm.issue((const char *const *)webs->owned, webs->owned_count)) {
        snprintf(error, capacity, "cannot record package AI state machines"); return 0;
    }
    return 1;
}
static int pr_fsm_bind(const pr_provider *view, char *error, size_t capacity)
{
    const sh_package_fsm_webs *webs = view ? view->fsm_webs : NULL;
    sh_fsm_web_binding *bindings = NULL;
    sh_fsm_web_table table = {0};
    int ok;
    if (!g_fsm.installed) return 1;
    if (!g_fsm.installed()) return !pr_fsm_content(view);
    if (pr_fsm_content(view)) {
        if (!(bindings = calloc(webs->count ? webs->count : 1, sizeof(*bindings)))) {
            snprintf(error, capacity, "cannot bind package AI state machines"); return 0;
        }
        for (size_t i = 0; i < webs->count; i++) {
            const sh_package_fsm_web *web = webs->webs + i;
            bindings[i] = (sh_fsm_web_binding){web->config, (const char *const *)web->behaviors, web->behavior_count,
                (const char *const *)web->from, (const char *const *)web->to, web->count,
                (const char *const *)web->extra, web->extra_count};
        }
        table = (sh_fsm_web_table){bindings, webs->count, (const char *const *)webs->owned, webs->owned_count};
    }
    ok = g_fsm.publish(pr_fsm_content(view) ? &table : NULL);
    free(bindings);
    if (!ok) snprintf(error, capacity, "package AI state-machine webs do not resolve against the registered declarations (backend log)");
    return ok;
}

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
static int pr_report_line(char **report, const char *line)
{
    size_t used = *report ? strlen(*report) : 0, add = strlen(line) + 1;
    char *grown;
    if (used > SIZE_MAX - add || !(grown = realloc(*report, used + add))) return 0;
    *report = grown; memcpy(grown + used, line, add);
    backend_log(line);
    return 1;
}
/* The library rejected these packages on their own bytes alone
 * (sh_package_compile_environment.invalid_own_bytes): a file identity,
 * declaration or contribution the product grammar cannot parse, or a
 * descriptor setting this build does not support. Library scopes compile
 * independently, so no peer, shared collection or map takes part in that
 * decision; the product build, the game's archives and its reflection are
 * fixed for the process, and failed reads or allocations never reject
 * content. The same tree therefore gets the same decision: it is left out
 * again with its report instead of being compiled a second time. Any change
 * to its files, and every new process, judges it afresh. Decisions that also
 * read stock composition, readers, coverage, native preparation or peers
 * (not served, conflicts, composition failures) are never remembered. */
static int pr_exclusions_apply(const pr_exclusion *remembered, size_t remembered_count, int earlier_process,
    sh_package_sources *sources, sh_package_sources *inventory, pr_exclusion **out, size_t *out_count,
    char **rejections)
{
    size_t reused = 0;
    for (size_t j = 0; j < remembered_count; j++) {
        const pr_exclusion *item = remembered + j;
        size_t i = 0;
        while (i < sources->package_count && (strcmp(sources->packages[i].name, item->name) ||
            memcmp(sources->fingerprints[i], item->tree, 32))) i++;
        if (i == sources->package_count) continue;
        /* Invalid content establishes no installed availability. */
        if (inventory) for (size_t k = 0; k < inventory->package_count; k++)
            if (!strcmp(inventory->packages[k].name, item->name)) {
                if (!sh_package_sources_remove(inventory, k)) return 0;
                break;
            }
        if (!pr_report_line(rejections, item->line) ||
            !pr_exclusion_add(out, out_count, item->name, item->tree, item->line) ||
            !sh_package_sources_remove(sources, i)) return 0;
        reused++;
    }
    if (reused) {
        char line[200];
        snprintf(line, sizeof(line), "package library: %zu unchanged package(s) left out as before%s, not compiled again",
            reused, earlier_process ? " (remembered by an earlier process of this product and game build)" : "");
        backend_log(line);
    }
    return 1;
}
#ifdef SH_PACKAGE_RUNTIME_TESTING
static char g_test_build_identity[256] = "game test\nproduct test\n";
void sh_package_runtime_test_build_identity(const char *identity)
{ snprintf(g_test_build_identity, sizeof(g_test_build_identity), "%s", identity ? identity : ""); }
static char g_test_image_slot[16] = "vk";
void sh_package_runtime_test_image_slot(const char *slot)
{ snprintf(g_test_image_slot, sizeof(g_test_image_slot), "%s", slot ? slot : ""); }
#endif
/* The images whose code makes these decisions: the game executable and this
 * product module, by link timestamp and image size. */
static int pr_build_identity(char *out, size_t capacity)
{
#ifdef SH_PACKAGE_RUNTIME_TESTING
    int n = snprintf(out, capacity, "%s", g_test_build_identity);
#else
    HMODULE self = NULL;
    const unsigned char *game = (const unsigned char *)GetModuleHandleW(NULL);
    if (!game || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(void *)pr_build_identity, &self) || !self) return 0;
    const unsigned char *product = (const unsigned char *)self;
    const IMAGE_NT_HEADERS *g = (const IMAGE_NT_HEADERS *)(game + ((const IMAGE_DOS_HEADER *)game)->e_lfanew);
    const IMAGE_NT_HEADERS *p = (const IMAGE_NT_HEADERS *)(product + ((const IMAGE_DOS_HEADER *)product)->e_lfanew);
    int n = snprintf(out, capacity, "game %08lx %08lx\nproduct %08lx %08lx\n",
        (unsigned long)g->FileHeader.TimeDateStamp, (unsigned long)g->OptionalHeader.SizeOfImage,
        (unsigned long)p->FileHeader.TimeDateStamp, (unsigned long)p->OptionalHeader.SizeOfImage);
#endif
    return n > 0 && (size_t)n < capacity;
}
/* A player alternates between the two renderers' executables, so each keeps
 * its own memo: a start on one never discards the other's decisions. An
 * unknown renderer remembers nothing. */
static int pr_exclusions_name(char *name, size_t capacity)
{
#ifdef SH_PACKAGE_RUNTIME_TESTING
    const char *slot = g_test_image_slot;
#else
    int vulkan = sh_host_is_vulkan();
    const char *slot = vulkan > 0 ? "vk" : vulkan == 0 ? "gl" : "";
#endif
    int n = snprintf(name, capacity, "own-bytes-exclusions-%s.txt", slot);
    return *slot && n > 0 && (size_t)n < capacity;
}
/* Own-bytes decisions outlive the process for the same product and game
 * images (pr_exclusions_apply): the same tree then reaches the same decision,
 * so a later process leaves it out without compiling it again. A different
 * image, a changed tree, or a missing, foreign, truncated or corrupt file is
 * judged afresh. Only this memo's own list is stored; nothing else is. */
static int pr_exclusions_file(const char *data_root, char *path, size_t path_capacity,
    char *header, size_t header_capacity)
{
    char identity[256], name[64];
    if (!pr_exclusions_name(name, sizeof(name)) || !pr_cache_state(data_root, name, path, path_capacity) ||
        !pr_build_identity(identity, sizeof(identity))) return 0;
    int m = snprintf(header, header_capacity, "snapmap-plus own-bytes exclusions v1\n%s", identity);
    return m > 0 && (size_t)m < header_capacity;
}
static int pr_unhex(const char *text, size_t length, unsigned char *out)
{
    for (size_t i = 0; i < length / 2; i++) {
        int hi = text[2 * i], lo = text[2 * i + 1];
        hi = hi >= '0' && hi <= '9' ? hi - '0' : hi >= 'a' && hi <= 'f' ? hi - 'a' + 10 : -1;
        lo = lo >= '0' && lo <= '9' ? lo - '0' : lo >= 'a' && lo <= 'f' ? lo - 'a' + 10 : -1;
        if (hi < 0 || lo < 0) return 0;
        out[i] = (unsigned char)(hi * 16 + lo);
    }
    return !(length % 2);
}
static void pr_exclusions_store(const char *data_root, const pr_exclusion *items, size_t count)
{
    char path[4352], header[320], name[64];
    size_t length = 1;
    wchar_t *wide;
    /* The unslotted memo of earlier builds is never read again. */
    if (pr_cache_state_existing(data_root, "own-bytes-exclusions.txt", path, sizeof(path)) &&
        (wide = sh_package_source_wide_path(path)) != NULL) { DeleteFileW(wide); free(wide); }
    if (!count) {
        /* Nothing to remember: remove an old memo, and create no state. */
        if (pr_exclusions_name(name, sizeof(name)) && pr_cache_state_existing(data_root, name, path, sizeof(path)) &&
            (wide = sh_package_source_wide_path(path)) != NULL) { DeleteFileW(wide); free(wide); }
        return;
    }
    for (size_t i = 0; i < count; i++) length += 2 * (strlen(items[i].name) + 32 + strlen(items[i].line)) + 3;
    char *body = malloc(length), *at = body, *text = NULL;
    if (!body || !pr_exclusions_file(data_root, path, sizeof(path), header, sizeof(header))) { free(body); return; }
    for (size_t i = 0; i < count; i++) {
        const unsigned char *fields[3] = {(const unsigned char *)items[i].name, items[i].tree,
            (const unsigned char *)items[i].line};
        size_t sizes[3] = {strlen(items[i].name), 32, strlen(items[i].line)};
        for (int f = 0; f < 3; f++) {
            for (size_t k = 0; k < sizes[f]; k++) { snprintf(at, 3, "%02x", fields[f][k]); at += 2; }
            *at++ = f < 2 ? '\t' : '\n';
        }
    }
    *at = 0;
    text = sh_package_framed_format(header, body); free(body);
    if (text) (void)pr_state_write(path, text);
    free(text);
}
static void pr_exclusions_load(const char *data_root, pr_exclusion **items, size_t *count)
{
    char path[4352], header[320], cache[4096];
    size_t length = 0, body_length = 0;
    *items = NULL; *count = 0;
    /* No package-cache yet: nothing was remembered (and none is created). */
    char identity[256], name[64];
    int m;
    (void)cache;
    if (!pr_exclusions_name(name, sizeof(name)) || !pr_cache_state_existing(data_root, name, path, sizeof(path)) ||
        !pr_build_identity(identity, sizeof(identity)) ||
        (m = snprintf(header, sizeof(header), "snapmap-plus own-bytes exclusions v1\n%s", identity)) <= 0 ||
        (size_t)m >= sizeof(header)) return;
    char *text = pr_state_read(path, 1 << 22, &length);
    if (!text) return;
    const char *body = sh_package_framed_body(text, length, header, &body_length), *end = body + body_length;
    int ok = body != NULL;
    for (const char *p = body; ok && p < end; ) {
        const char *stop = memchr(p, '\n', (size_t)(end - p)), *tab1, *tab2;
        ok = stop && (tab1 = memchr(p, '\t', (size_t)(stop - p))) && (tab2 = memchr(tab1 + 1, '\t', (size_t)(stop - tab1 - 1)));
        if (!ok) break;
        size_t n = (size_t)(tab1 - p), t = (size_t)(tab2 - tab1 - 1), l = (size_t)(stop - tab2 - 1);
        char *name = malloc(n / 2 + 1), *line = malloc(l / 2 + 1);
        unsigned char tree[32];
        ok = name && line && n && n % 2 == 0 && t == 64 && l % 2 == 0 && pr_unhex(p, n, (unsigned char *)name) &&
            pr_unhex(tab1 + 1, t, tree) && pr_unhex(tab2 + 1, l, (unsigned char *)line);
        if (ok) {
            name[n / 2] = 0; line[l / 2] = 0;
            ok = strlen(name) == n / 2 && strlen(line) == l / 2 && pr_exclusion_add(items, count, name, tree, line);
        }
        free(name); free(line);
        p = stop + 1;
    }
    free(text);
    if (!ok) {
        pr_exclusions_free(*items, *count); *items = NULL; *count = 0;
        backend_log("package library: remembered own-bytes exclusions not used (another product or game build, "
            "or an incomplete or corrupt record); packages are judged afresh");
    }
}
/* Leave one invalid package out of this library compile: it establishes no
 * installed availability, reports why, and is remembered when the decision
 * read only its own bytes (pr_exclusions_apply). */
static int pr_reject_invalid(sh_package_sources *sources, sh_package_sources *inventory, size_t package,
    int own_bytes, char **rejections, pr_exclusion **exclusions, size_t *exclusion_count, const char *reason)
{
    if (package >= sources->package_count) return 0;
    if (inventory) for (size_t i = 0; i < inventory->package_count; i++)
        if (!strcmp(inventory->packages[i].name, sources->packages[package].name)) {
            if (!sh_package_sources_remove(inventory, i)) return 0;
            break;
        }
    size_t reported = *rejections ? strlen(*rejections) : 0;
    return pr_rejected(rejections, &sources->packages[package], reason) &&
        (!own_bytes || pr_exclusion_add(exclusions, exclusion_count, sources->packages[package].name,
            sources->fingerprints[package], *rejections + reported)) &&
        sh_package_sources_remove(sources, package);
}

/* Leave out, in one step, every package one failed compile found responsible
 * on its own: the reported one and the further withdrawals of the same capture
 * (sh_package_compile_environment.more_rejections). Invalid packages lose
 * installed availability and are remembered when their own bytes decided it;
 * packages this build cannot serve yet stay available. own_bytes_only applies
 * only own-byte rejections (the others are judged again after native
 * preparation). Removal runs from the highest index so indices stay valid. */
typedef struct pr_leave { size_t package; int invalid, own_bytes; const char *reason; } pr_leave;
static int pr_leave_order(const void *a, const void *b)
{
    const pr_leave *x = a, *y = b;
    if (x->package != y->package) return x->package < y->package ? 1 : -1;
    return y->invalid - x->invalid;
}
static int pr_leave_out(sh_package_sources *sources, sh_package_sources *inventory,
    size_t invalid_package, int invalid_own_bytes, size_t unserved_package, const char *reason,
    const sh_package_rejections *more, int own_bytes_only, char **rejections, char **unserved,
    pr_exclusion **exclusions, size_t *exclusion_count)
{
    size_t count = 0;
    pr_leave *leave = calloc(2 + (more ? more->count : 0), sizeof(*leave));
    if (!leave) return 0;
    if (invalid_package != SIZE_MAX && (!own_bytes_only || invalid_own_bytes))
        leave[count++] = (pr_leave){invalid_package, 1, invalid_own_bytes, reason};
    if (unserved_package != SIZE_MAX && !own_bytes_only)
        leave[count++] = (pr_leave){unserved_package, 0, 0, reason};
    for (size_t i = 0; more && i < more->count; i++) {
        const sh_package_rejection *r = more->items + i;
        if (own_bytes_only && !(r->invalid && r->own_bytes)) continue;
        leave[count++] = (pr_leave){r->package, r->invalid, r->invalid && r->own_bytes, r->reason};
    }
    qsort(leave, count, sizeof(*leave), pr_leave_order);
    int ok = 1;
    for (size_t i = 0; ok && i < count; i++) {
        if (i && leave[i].package == leave[i - 1].package) continue;
        if (leave[i].package >= sources->package_count) { ok = 0; break; }
        ok = leave[i].invalid ?
            pr_reject_invalid(sources, inventory, leave[i].package, leave[i].own_bytes, rejections,
                exclusions, exclusion_count, leave[i].reason) :
            pr_unserved(unserved, &sources->packages[leave[i].package], leave[i].reason) &&
                sh_package_sources_remove(sources, leave[i].package);
    }
    free(leave);
    return ok;
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

/* The typed resource values a legacy document names in its historical
 * composition. Parent and class lookups are not values; keys name no
 * resource; authored rows are already in the recipe. */
typedef struct pr_document_walk {
    sh_package_references *roots;
    const sh_package_provenance *authored;
} pr_document_walk;
static int pr_document_lookup(void *context, const sh_package_provenance_site *site,
    const char *family, const char *name, const char **replacement)
{ (void)context; (void)site; (void)family; (void)name; *replacement = NULL; return 0; }
static int pr_document_value(void *context, const sh_package_provenance_site *site,
    const char *family, const char *name, const char **replacement)
{
    pr_document_walk *walk = context; (void)site; *replacement = NULL;
    if (!strcmp(family, SH_PACKAGE_KEY_FAMILY)) return 0;
    for (size_t i = 0; i < walk->authored->binding_count; i++)
        if (!_stricmp(walk->authored->bindings[i].family, family) &&
            !_stricmp(walk->authored->bindings[i].original, name)) return 0;
    for (size_t i = 0; i < walk->roots->count; i++)
        if (!strcmp(walk->roots->items[i].type, family) && !strcmp(walk->roots->items[i].name, name)) return 0;
    return sh_package_references_add(walk->roots, family, name) ? 0 : -1;
}
static int pr_context_equal(const sh_package_provenance_context *a,
    const sh_package_provenance_context *b);
/* A legacy context changes stock identities it never authored: a changed bank
 * changes the sound declarations naming its events, and a changed material,
 * model or image changes the entities that reach it. Under the file
 * replacement the map was authored with, those stock names meant the changed
 * resources. The compiled inventory binds them in the map's own historical
 * context; retain each changed one the document names, so that its site can
 * select it. Authored rows are never replaced, an unchanged identity gains no
 * row, another context's binding is never borrowed, and a row is not a use. */
static int pr_historical_derived(sh_package_provenance *historical, const sh_package_provenance *linked,
    const sh_package_references *roots)
{
    for (size_t r = 0; roots && r < roots->count; r++) {
        const char *family = roots->items[r].type, *name = roots->items[r].name;
        for (size_t i = 0; i < linked->binding_count; i++) {
            const sh_package_provenance_binding *binding = linked->bindings + i;
            if (_stricmp(binding->family, family) || _stricmp(binding->original, name) ||
                !_stricmp(binding->name, binding->original)) continue;
            for (size_t k = 0; k < binding->context_count; k++) {
                size_t h, present = 0;
                if (binding->contexts[k] >= linked->context_count) return 0;
                for (h = 0; h < historical->context_count; h++)
                    if (pr_context_equal(historical->contexts + h, linked->contexts + binding->contexts[k])) break;
                if (h == historical->context_count) continue;
                for (size_t j = 0; j < historical->binding_count && !present; j++)
                    present = !_stricmp(historical->bindings[j].family, family) &&
                        !_stricmp(historical->bindings[j].original, name);
                if (!present && !sh_package_provenance_bind(historical, binding->family, binding->original,
                    binding->original, &h, 1)) return 0;
            }
        }
    }
    return 1;
}
#ifdef SH_PACKAGE_RUNTIME_TESTING
int sh_package_runtime_test_historical_derived(sh_package_provenance *historical,
    const sh_package_provenance *linked, const sh_package_references *roots)
{ return pr_historical_derived(historical, linked, roots); }
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

#ifndef SH_PACKAGE_RUNTIME_TESTING
/* Cooked OpenGL programs name further programs (program-typed parameters)
 * that a compilation reaches only by reading the cooked output, one level per
 * full compile. Make now the requests the next compile would make on reaching
 * them, so one restart consumes the whole reference closure. A failed
 * follow-up drops captured output like any failed preparation: the compile
 * then requests what it needs itself, so the caller forgets the completed
 * task identities with it (0 returned) and follows nothing further. */
static int pr_follow_native_sources(sh_package_native_assets *assets, uint64_t **tasks, size_t *task_count)
{
    uint64_t *followed = NULL;
    size_t followed_count = 0, queued, total = 0;
    int rounds = 0, ok = 1;
    char error[1024] = "", line[1300];
    while (rounds < 32 && (queued = sh_package_native_assets_shader_follow(assets)) != 0) {
        rounds++; total += queued;
        if (!pr_prepare_native_sources(assets, &followed, &followed_count, error, sizeof(error))) {
            if (tasks && task_count) { free(*tasks); *tasks = NULL; *task_count = 0; }
            ok = 0; break;
        }
    }
    free(followed);
    if (total) {
        snprintf(line, sizeof(line), "shader preparation followed %zu program(s) named by cooked programs in %d round(s)%s%s",
            total, rounds, ok ? "" : "; not prepared, the compile requests what it needs: ", ok ? "" : error);
        backend_log(line);
    }
    return ok;
}
/* Installed inventory identity for the preparation hint: package names and
 * source tree fingerprints, in name order. */
static int pr_inventory_digest(const sh_package_sources *sources, unsigned char digest[32])
{
    size_t count = sources ? sources->package_count : 0, length = 0;
    size_t *order = calloc(count ? count : 1, sizeof(*order));
    unsigned char *buffer = NULL, *at;
    sh_package_file_identity identity;
    int ok = 0;
    if (!order || (count && !sources->fingerprints)) goto done;
    for (size_t i = 0; i < count; i++) {
        size_t j = i;
        while (j && strcmp(sources->packages[order[j - 1]].name, sources->packages[i].name) > 0) { order[j] = order[j - 1]; j--; }
        order[j] = i; length += strlen(sources->packages[i].name) + 1 + 32;
    }
    if (!(buffer = malloc(length ? length : 1))) goto done;
    at = buffer;
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(sources->packages[order[i]].name) + 1;
        memcpy(at, sources->packages[order[i]].name, n); at += n;
        memcpy(at, sources->fingerprints[order[i]], 32); at += 32;
    }
    ok = sh_package_bytes_identity(buffer, length, &identity);
    if (ok) memcpy(digest, identity.digest, 32);
done:
    free(buffer); free(order); return ok;
}
/* Shader preparation hint: the stock shader objects the previous installed-
 * library compile needed (sh_package_native_assets_shader_needs). It belongs to
 * exactly one renderer, game image, product build and installed inventory; any
 * other header, and any truncated or corrupt text, is not used. A used hint
 * only prepares ahead of the first library compile of a session: the compile
 * still verifies every object and requests what it needs exactly as without
 * one, so a missing or rejected hint costs only the discovery it would save. */
static int pr_shader_hint(const char *data_root, const unsigned char inventory[32],
    char *path, size_t path_capacity, char *header, size_t header_capacity)
{
    char identity[65], build[256];
    int vulkan = sh_host_is_vulkan();
    if (vulkan < 0 || !pr_cache_state(data_root, vulkan ? "shader-preparation-vk.txt" : "shader-preparation-gl.txt",
            path, path_capacity) || !pr_build_identity(build, sizeof(build))) return 0;
    for (size_t i = 0; i < 32; i++) snprintf(identity + 2 * i, 3, "%02x", inventory[i]);
    int n = 1;
    int m = snprintf(header, header_capacity, "snapmap-plus shader preparation v2\nrenderer %s\n%sinventory %s\n",
        vulkan ? "vulkan" : "opengl", build, identity);
    return n > 0 && (size_t)n < path_capacity && m > 0 && (size_t)m < header_capacity;
}
typedef struct pr_prewarm_walk { sh_package_native_assets *assets; size_t listed, queued, stale; } pr_prewarm_walk;
static int pr_prewarm_one(void *context, const char *family, const char *name)
{
    pr_prewarm_walk *walk = context;
    int status = sh_package_native_assets_shader_prewarm(walk->assets, family, name);
    walk->listed++;
    if (status > 0) walk->queued++;
    else if (status < 0) walk->stale++;
    return 1;
}
static void pr_shader_prewarm(sh_package_native_assets *assets, const char *data_root, const unsigned char inventory[32])
{
    char path[4352], header[512], error[1024] = "", line[1400];
    size_t length = 0;
    pr_prewarm_walk walk = {assets, 0, 0, 0};
    if (!pr_shader_hint(data_root, inventory, path, sizeof(path), header, sizeof(header))) return;
    wchar_t *wide = sh_package_source_wide_path(path);
    int present = wide && GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
    free(wide);
    if (!present) {
        backend_log("shader preparation: no hint yet for this renderer; the library compile discovers what it needs");
        return;
    }
    char *text = pr_state_read(path, 1 << 20, &length);
    int entries = text ? sh_package_shader_hint_read(text, length, header, pr_prewarm_one, &walk) : -1;
    free(text);
    if (entries < 0) {
        backend_log("shader preparation hint not used: it belongs to another renderer, game, product build or "
            "installed inventory, or is incomplete; the library compile discovers what it needs");
        return;
    }
    uint64_t *tasks = NULL;
    size_t task_count = 0;
    int prepared = !walk.queued || pr_prepare_native_sources(assets, &tasks, &task_count, error, sizeof(error));
    free(tasks);
    if (prepared && walk.queued) prepared = pr_follow_native_sources(assets, NULL, NULL);
    snprintf(line, sizeof(line), "shader preparation ahead of the first library compile: %zu of %zu hinted object(s) "
        "queued, %zu without an installed original%s%s", walk.queued, walk.listed, walk.stale,
        prepared ? "" : "; not prepared, the compile requests what it needs: ", prepared ? "" : error);
    backend_log(line);
}
static void pr_shader_hint_write(sh_package_native_assets *assets, const char *data_root, const unsigned char inventory[32])
{
    char path[4352], header[512];
    char *needs = sh_package_native_assets_shader_needs(assets), *text = NULL;
    if (!needs || !pr_shader_hint(data_root, inventory, path, sizeof(path), header, sizeof(header)) ||
        !(text = sh_package_shader_hint_format(header, needs))) { free(needs); free(text); return; }
    (void)pr_state_write(path, text);
    free(needs); free(text);
}
#endif

/* authoring_increment: an installation's newly captured packages, or an
 * installed-library refresh that must only add packages to previous (else
 * *composition_failed is -1 and nothing is compiled). */
enum { PR_INCREMENT_INSTALL = 1, PR_INCREMENT_REFRESH = 2 };
/* Diagnostic identity of the published library: each published resource's
 * engine path, declaration and manager identities (aliases included), original
 * path and served payload identity (presented text where it has one), in
 * engine-path order. Equal inventories under equal product semantics give an
 * equal digest, so a performance change must leave it unchanged. 0 when a
 * payload has no captured identity or memory runs out. */
static int pr_digest_order(void *context, const void *left, const void *right)
{
    const sh_package_compilation *compiled = context;
    const sh_compiled_resource *a = compiled->resources + *(const size_t *)left;
    const sh_compiled_resource *b = compiled->resources + *(const size_t *)right;
    return strcmp(a->engine_path ? a->engine_path : "", b->engine_path ? b->engine_path : "");
}
static int pr_digest_append(unsigned char **buffer, size_t *used, size_t *capacity, const void *data, size_t length)
{
    if (length > SIZE_MAX - *used) return 0;
    if (*used + length > *capacity) {
        size_t next = *capacity ? *capacity : 65536;
        while (next < *used + length) { if (next > SIZE_MAX / 2) return 0; next *= 2; }
        unsigned char *grown = realloc(*buffer, next);
        if (!grown) return 0;
        *buffer = grown; *capacity = next;
    }
    memcpy(*buffer + *used, data, length); *used += length; return 1;
}
static int pr_digest_text(unsigned char **buffer, size_t *used, size_t *capacity, const char *text)
{
    const char *value = text ? text : "";
    return pr_digest_append(buffer, used, capacity, value, strlen(value) + 1);
}
static int pr_library_digest(const sh_package_compilation *compiled, char hex[65])
{
    size_t *order = calloc(compiled->resource_count ? compiled->resource_count : 1, sizeof(*order));
    unsigned char *buffer = NULL;
    size_t used = 0, capacity = 0;
    sh_package_file_identity identity;
    int ok = order != NULL;
    hex[0] = 0;
    for (size_t i = 0; ok && i < compiled->resource_count; i++) order[i] = i;
    if (ok && compiled->resource_count > 1)
        qsort_s(order, compiled->resource_count, sizeof(*order), pr_digest_order, (void *)compiled);
    for (size_t k = 0; ok && k < compiled->resource_count; k++) {
        const sh_compiled_resource *r = compiled->resources + order[k];
        ok = pr_digest_text(&buffer, &used, &capacity, r->engine_path) &&
            pr_digest_text(&buffer, &used, &capacity, r->type) && pr_digest_text(&buffer, &used, &capacity, r->name) &&
            pr_digest_text(&buffer, &used, &capacity, r->native_type) &&
            pr_digest_text(&buffer, &used, &capacity, r->native_name) &&
            pr_digest_text(&buffer, &used, &capacity, r->original_path);
        for (size_t a = 0; ok && a < r->native_alias_count; a++)
            ok = pr_digest_text(&buffer, &used, &capacity, r->native_aliases[a].type) &&
                pr_digest_text(&buffer, &used, &capacity, r->native_aliases[a].name);
        if (ok && r->presented) ok = sh_package_bytes_identity(r->presented, r->presented_length, &identity);
        else if (ok) ok = sh_package_compilation_identity(compiled, r, &identity, NULL, 0);
        if (ok) ok = pr_digest_append(&buffer, &used, &capacity, &identity, sizeof(identity));
    }
    if (ok) ok = sh_package_bytes_identity(buffer ? buffer : (const unsigned char *)"", used, &identity);
    if (ok) for (int b = 0; b < 32; b++) snprintf(hex + 2 * b, 3, "%02x", identity.digest[b]);
    free(order); free(buffer);
    return ok;
}
static pr_provider *pr_prepare_deliveries(const char *data_root, const char *source_root,
    const pr_provider *previous, int product_only, int authoring_increment, pr_inventory **inventory_out,
    int *composition_failed, const sh_package_provenance *contexts,
    const sh_package_bundle_binding *deliveries, size_t delivery_count, int use_deliveries,
    const pr_provider *string_peer, const char *document, size_t document_length,
    char *out_error, size_t out_capacity)
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
    int fresh_assets = 0, hint_ready = 0, following = 1;
    unsigned char hint_inventory[32];
    sh_package_native_assets *recording = NULL;
#else
    (void)string_peer;
#endif
    DWORD n;
    size_t default_index;
    char *slash, *rejections = NULL, *unserved = NULL;
    pr_exclusion *exclusions = NULL;
    size_t exclusion_count = 0;
    size_t invalid_package = SIZE_MAX, unserved_package = SIZE_MAX;
    int invalid_own_bytes = 0;
    sh_package_rejections more_rejections = {0};
    sh_package_owners conflicted = {0};
    sh_package_library isolated = {0};
    int library_output = 0;
    sh_package_provenance historical = {0}, authoring_contexts = {0};
    sh_package_references document_roots = {0};
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
#ifndef SH_PACKAGE_RUNTIME_TESTING
    /* The inventory as scanned, before anything is left out, identifies the
     * shader preparation hint this compile reads and writes. */
    if (!source_root && !product_only && !authoring_increment)
        hint_ready = pr_inventory_digest(sources, hint_inventory);
#endif
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
        /* These are immutable installed originals and witnessed source-cook
         * products, never an authored provider's files. A fresh map has no
         * previous map provider after leaving the browser: share the library's
         * retained originals instead of cooking them again on every map load.
         * Each shader ABI read still verifies the current native identity. */
        const pr_provider *native_owner = previous && previous->native_assets ? previous : g_library_provider;
        native_assets = native_owner && native_owner->native_assets ?
            sh_package_native_assets_retain(native_owner->native_assets) : sh_package_native_assets_open(doom_root);
        if (!native_assets) goto done;
        if (!native_owner || !native_owner->native_assets) {
            fresh_assets = 1;
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
        environment.invalid_own_bytes = &invalid_own_bytes;
        environment.unserved_package = &unserved_package;
        environment.more_rejections = &more_rejections;
        environment.conflicted_packages = &conflicted;
    }
    if (inventory_out) {
        sh_package_sources empty = {0};
        inventory_sources = sh_package_sources_join(sources, &empty, error, sizeof(error));
        if (!inventory_sources) goto done;
    }
    if (!source_root && !product_only && previous && previous->exclusion_count &&
        !pr_exclusions_apply(previous->exclusions, previous->exclusion_count, 0, sources, inventory_sources,
            &exclusions, &exclusion_count, &rejections)) {
        snprintf(error, sizeof(error), "cannot retain packages left out for their own content"); goto done;
    }
    if (!source_root && !product_only && !authoring_increment && !previous) {
        pr_exclusion *remembered = NULL;
        size_t remembered_count = 0;
        pr_exclusions_load(data_root, &remembered, &remembered_count);
        int applied = !remembered_count || pr_exclusions_apply(remembered, remembered_count, 1, sources,
            inventory_sources, &exclusions, &exclusion_count, &rejections);
        pr_exclusions_free(remembered, remembered_count);
        if (!applied) { snprintf(error, sizeof(error), "cannot retain packages left out for their own content"); goto done; }
    }
    int authoring_linked = 0;
    if (authoring_increment) {
        int linked_sources = reflection != 0;
#ifdef SH_PACKAGE_RUNTIME_TESTING
        linked_sources = sh_package_runtime_test_source_inputs(NULL, &reflection, NULL, NULL);
#endif
        if (linked_sources && authoring_increment == PR_INCREMENT_REFRESH) {
            /* A refresh reuses the retained library only when it merely adds
             * packages: a removed or changed one needs the whole library. */
            for (size_t j = 0; previous && j < previous->sources->package_count; j++) {
                int kept = 0;
                for (size_t i = 0; i < sources->package_count && !kept; i++)
                    kept = !strcmp(sources->packages[i].name, previous->sources->packages[j].name) &&
                        !memcmp(sources->fingerprints[i], previous->sources->fingerprints[j], 32);
                if (!kept) {
                    if (composition_failed) *composition_failed = -1;
                    snprintf(error, sizeof(error), "installed package %s was removed or changed",
                        previous->sources->packages[j].name);
                    goto done;
                }
            }
        }
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
            authoring_linked = 1;
            environment.previous = NULL;
            /* Each new component compiles alone (an isolated context), so a
             * failure of its own source or product coverage names its package,
             * which is left out or not served as a whole-library compile would;
             * healthy new peers continue. No installed owner takes part, so
             * none can conflict. Environment failures still blame nobody. */
            environment.conflicted_packages = NULL;
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
        if (document) {
            sh_decl_registry_source document_registry = {0};
            uintptr_t document_reflection = reflection;
            pr_original_source document_originals = {baseline, baseline_context, native_assets};
            int ready;
#ifndef SH_PACKAGE_RUNTIME_TESTING
            ready = sh_decl_server_registry_source(&document_registry);
#else
            ready = sh_package_runtime_test_source_inputs(&document_registry, &document_reflection, NULL, NULL);
#endif
            sh_package_source_reader *reader = ready ? sh_package_source_reader_open(compiled, catalog,
                pr_original, &document_originals, document_registry, document_reflection, error, sizeof(error)) : NULL;
            pr_document_walk walk = {&document_roots, &historical};
            size_t walked = 0;
            char *copy = reader ? sh_package_source_bind_map_sites_observed(reader, document, document_length,
                pr_document_lookup, pr_document_value, &walk, &walked, NULL, error, sizeof(error)) : NULL;
            sh_package_source_reader_close(reader);
            if (!copy) {
                if (!error[0]) snprintf(error, sizeof(error), "cannot read the map's typed resource values");
                goto done;
            }
            free(copy);
        }
        sh_package_compilation_free(compiled); compiled = NULL;
        contexts = &historical;
    }
#ifndef SH_PACKAGE_RUNTIME_TESTING
    if (native_assets && hint_ready) {
        if (fresh_assets) pr_shader_prewarm(native_assets, data_root, hint_inventory);
        recording = native_assets; sh_package_native_assets_shader_record(recording, 1);
    }
#endif
    for (;;) {
        if (authoring_linked) {
            /* One isolated context per new component, again after any is left out. */
            sh_package_provenance_free(&authoring_contexts);
            for (size_t i = 0; i < sources->component_count; i++) {
                const sh_package_component *component = sources->components + i;
                const unsigned char *tree = sh_package_source_tree(sources, component->owner);
                if (!tree || sh_package_provenance_context_add(&authoring_contexts,
                    SH_PACKAGE_CONTEXT_ISOLATED, (const unsigned char (*)[32])tree, 1,
                    component->relative) == SIZE_MAX) goto done;
            }
            contexts = authoring_contexts.context_count ? &authoring_contexts : NULL;
            if (!contexts) environment.builtin_count = 0;
        }
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
            pr_compile_rows shipped = {catalog, &originals};
            native.editor_rows = pr_compile_rows_read; native.editor_rows_context = &shipped;
            if (!g_source_memo) g_source_memo = sh_package_source_memo_open(); /* Optional. */
            native.source.memo = g_source_memo;
#ifndef SH_PACKAGE_RUNTIME_TESTING
            native.private_keys = sh_spawn_keys_active();
#else
            native.private_keys = g_test_spawn_keys;
#endif
            /* Installed packages keep the registration keys the installed
             * library gave them: a map compile keys installed scopes as that
             * library does, and a library recompile keeps its live keys. */
            {
                const pr_provider *keys = g_current_provider ? g_current_provider : source_root ? string_peer : previous;
                native.keys = keys ? &keys->bindings : NULL;
                native.key_groups = keys ? keys->key_groups : NULL;
            }
            /* Audio compiles into one retained program per provider. Native
             * activation publishes that program; there is no old-path fallback. */
            native.audio = audio_originals;
            if (contexts == &historical && document_roots.count) native.document_roots = &document_roots;
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
            if (!g_link_history) {
                char directory[32768];
                if (!pr_cache_directory(data_root, directory, sizeof(directory)) ||
                    !(g_link_history = sh_package_link_history_open(directory))) {
                    snprintf(error, sizeof(error), "cannot retain native package identity history"); goto done;
                }
            }
            native.history = g_link_history;
            if (!sh_decl_server_registry_source(&registry)) {
                snprintf(error, sizeof(error), "native source registry is unavailable at package compilation");
                goto done;
            }
#else
            if (!sh_package_runtime_test_source_inputs(&registry, &reflection, NULL, NULL)) {
                snprintf(error, sizeof(error), "native map bindings require registered source readers"); goto done;
            }
            native.source.implicit = g_test_constructor ? pr_test_constructor : NULL;
            native.strings = g_test_strings;
#endif
            ULONGLONG started = GetTickCount64();
            char scope_error[512], line[1280];
            sh_package_native_assets_shader_scope_begin(native_assets);
            int linked = contexts ? sh_package_library_compile_contexts(sources, contexts, &environment,
                catalog, pr_original, &originals, registry, reflection, &native, &isolated, error, sizeof(error)) :
                sh_package_library_compile(sources, &environment, catalog, pr_original, &originals,
                    registry, reflection, &native, &isolated, error, sizeof(error));
            sh_editor_rows_free(&shipped.rows);
            if (!sh_package_native_assets_shader_scope_end(native_assets, scope_error, sizeof(scope_error)) && linked) {
                /* Native identities changed under this candidate: environmental. */
                sh_package_library_free(&isolated); linked = 0;
                strcpy_s(error, sizeof(error), scope_error);
            }
            snprintf(line, sizeof(line), "package library compile (%s): %s in %llu ms%s%s",
                authoring_increment == PR_INCREMENT_REFRESH ? "installed library increment" :
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
            for (const char *at = isolated.repeated_work; *at;) {
                const char *end = strchr(at, '\n');
                size_t length = end ? (size_t)(end - at) : strlen(at);
                snprintf(line, sizeof(line), "package library repeated work: %.*s", (int)length, at);
                backend_log(line);
                at += length + (end ? 1 : 0);
            }
            /* Accounted coverage gaps help diagnose a later refusal too. */
            if (!linked && isolated.coverage) backend_log(isolated.coverage);
            /* A refused native identity names both of its claimants. */
            for (const char *at = isolated.link_diagnostics; at && *at;) {
                const char *end = strchr(at, '\n');
                size_t length = end ? (size_t)(end - at) : strlen(at);
                snprintf(line, sizeof(line), "package library diagnostic: %.*s", (int)length, at);
                backend_log(line);
                at += length + (end ? 1 : 0);
            }
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
            char reason[sizeof(error)];
            strcpy_s(reason, sizeof(reason), error);
            if (!pr_prepare_native_sources(native_assets, &prepared_tasks, &prepared_task_count,
                error, sizeof(error))) goto done;
            if (following) following = pr_follow_native_sources(native_assets, &prepared_tasks, &prepared_task_count);
            /* A rejection read from the package's own bytes does not depend on
             * native shader state: act on it now instead of compiling it again
             * after the preparation. Other attributions are judged afresh. */
            if (!pr_leave_out(sources, inventory_sources, invalid_package, invalid_own_bytes, SIZE_MAX, reason,
                    &more_rejections, 1, &rejections, &unserved, &exclusions, &exclusion_count)) goto done;
            sh_package_library_free(&isolated);
            invalid_package = unserved_package = SIZE_MAX; sh_package_owners_free(&conflicted);
            error[0] = 0;
            continue;
        }
#endif
        if (unserved_package != SIZE_MAX || invalid_package != SIZE_MAX) {
            /* An unserved package's files are valid and stay available to map
             * installation; invalid content cannot establish installed
             * availability. Valid peers that only disagree retain their
             * physical inventory: a map's authored policy may use these files
             * without reinstalling. Every independent package the capture
             * withdrew goes in this same step. */
            if (!pr_leave_out(sources, inventory_sources, invalid_package, invalid_own_bytes, unserved_package,
                    error, &more_rejections, 0, &rejections, &unserved, &exclusions, &exclusion_count)) goto done;
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
    if (contexts == &historical && document_roots.count &&
        !pr_historical_derived(&historical, &isolated.bindings, &document_roots)) {
        snprintf(error, sizeof(error), "cannot retain the map's historical derived bindings"); goto done;
    }
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
        char line[256], digest[65];
        if (!pr_library_digest(compiled, digest)) strcpy_s(digest, sizeof(digest), "unavailable");
        snprintf(line, sizeof(line), "package library: %zu published resources; %zu unchanged SnapMap resources stay native; "
            "published digest %s", compiled->resource_count, native, digest);
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
    provider->fsm_webs = isolated.fsm_webs; isolated.fsm_webs = NULL;
    provider->audio = isolated.audio; isolated.audio = NULL;
    provider->audio_originals = audio_originals; audio_originals = NULL;
#ifndef SH_PACKAGE_RUNTIME_TESTING
    if (recording) {
        sh_package_native_assets_shader_record(recording, 0); recording = NULL;
        pr_shader_hint_write(native_assets, data_root, hint_inventory);
    }
#endif
    provider->native_assets = native_assets; native_assets = NULL;
#ifndef SH_PACKAGE_RUNTIME_TESTING
    provider->strings = strings; strings = NULL;
    provider->audio_ids = audio_ids; audio_ids = NULL;
#endif
    provider->references = 1;
    provider->rejections = rejections; rejections = NULL;
    if (!source_root && !product_only && !authoring_increment)
        pr_exclusions_store(data_root, exclusions, exclusion_count);
    provider->exclusions = exclusions; provider->exclusion_count = exclusion_count;
    exclusions = NULL; exclusion_count = 0;
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
#ifndef SH_PACKAGE_RUNTIME_TESTING
    if (recording) sh_package_native_assets_shader_record(recording, 0);
#endif
    sh_package_native_assets_close(native_assets);
    sh_decl_native_schema_close(native_schema);
    sh_package_sources_free(inventory_sources); sh_package_owners_free(&conflicted);
    sh_package_provenance_free(&historical); sh_package_references_free(&document_roots);
    sh_package_provenance_free(&authoring_contexts);
#ifndef SH_PACKAGE_RUNTIME_TESTING
    sh_strids_reservation_free(strings);
    free(prepared_tasks);
#endif
    free(rejections); free(unserved); pr_exclusions_free(exclusions, exclusion_count);
    sh_package_rejections_free(&more_rejections);
    return provider;
}

static pr_provider *pr_prepare(const char *data_root, const char *source_root,
    const pr_provider *previous, int product_only, pr_inventory **inventory_out,
    int *composition_failed, const sh_package_provenance *contexts,
    const pr_provider *string_peer, char *error, size_t capacity)
{
    return pr_prepare_deliveries(data_root, source_root, previous, product_only,
        0, inventory_out, composition_failed, contexts, NULL, 0, 0, string_peer, NULL, 0, error, capacity);
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
static int pr_resource_family(const sh_compiled_resource *resource, const char *family)
{
    for (size_t i = 0; i < sh_package_resource_native_count(resource); i++) {
        sh_package_native_identity identity = sh_package_resource_native_at(resource, i);
        if (identity.type && !_stricmp(identity.type, family)) return 1;
    }
    return 0;
}
sh_package_selector_hold *sh_package_runtime_selectors_retain(const char *family, char *error, size_t capacity)
{
    sh_package_selector_hold *hold = NULL;
    if (error && capacity) *error = 0;
    if (!family || !*family) {
        if (error && capacity) snprintf(error, capacity, "native selectors require their root resource family");
        return NULL;
    }
    if (!g_activation_callback) AcquireSRWLockExclusive(&g_refresh_lock);
    AcquireSRWLockShared(&g_lock);
    if (!g_current_provider || (!g_usable && !g_activation_callback)) goto done;
    size_t root;
    for (root = 0; root < g_current_provider->compiled->resource_count; root++)
        if (pr_resource_family(g_current_provider->compiled->resources + root, family)) break;
    if (root == g_current_provider->compiled->resource_count) goto done;
    hold = calloc(1, sizeof(*hold));
    if (!hold) goto done;
    hold->family = _strdup(family);
    if (!hold->family) { free(hold); hold = NULL; goto done; }
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
        *selector = hold->next; pr_release(hold->provider); free(hold->family); free(hold);
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

static int pr_selector_resource(const pr_provider *provider, const sh_compiled_resource *resource)
{
    for (const sh_package_selector_hold *hold = g_selectors; hold; hold = hold->next)
        if (hold->provider == provider && InterlockedCompareExchange((volatile LONG *)&hold->live, 0, 0) &&
            pr_resource_family(resource, hold->family)) return 1;
    return 0;
}
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
    for (size_t i = 0; i < count; i++) if (mode == PR_RETAIN_SELECTORS ?
        pr_selector_resource(provider, provider->compiled->resources + i) : (mode == PR_RETAIN_AUTHORING ?
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
static int pr_lease_behavior(void *context, const char *name) { return pr_lease_consumer(context, "aibehavior", name); }
/* A retained behavior keeps its web's generated first declaration and the
 * placeholder: neither is a bound dependency of authored content. */
static void pr_lease_fsm_declarations(pr_retained_selection *selection)
{
    const pr_provider *provider = selection->provider;
    const sh_package_fsm_webs *webs = provider->fsm_webs;
    for (size_t w = 0; webs && w < webs->count; w++) {
        int used = 0;
        for (size_t k = 0; k < webs->webs[w].behavior_count && !used; k++) used = pr_lease_behavior(selection, webs->webs[w].behaviors[k]);
        for (size_t i = 0; used && i < provider->compiled->resource_count; i++) {
            const sh_compiled_resource *resource = provider->compiled->resources + i;
            if (sh_package_resource_native_matches(resource, "aifsmmanager", webs->webs[w].to[0]) ||
                sh_package_resource_native_matches(resource, "aifsmmanager", SH_PACKAGE_FSM_PLACEHOLDER))
                selection->resources[i] = 1;
        }
    }
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
        /* Numeric bindings have no physical resource. Their immutable code
         * dictionary is retained with this provider; the complete context
         * bundle check below still controls which recipes can be captured. */
        int served = !strcmp(binding->family, SH_PACKAGE_KEY_FAMILY) ? 1 :
            pr_binding_served(provider, serving, binding, error, capacity);
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
/* Whether the provider publishes a resource under this exact native name. */
static int pr_provider_names(const pr_provider *provider, const sh_package_provenance_binding *binding)
{
    for (size_t i = 0; i < provider->compiled->resource_count; i++)
        if (sh_package_resource_native_matches(provider->compiled->resources + i, binding->family, binding->name)) return 1;
    return 0;
}
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
            /* A binding to the native identity itself that the provider does not
             * publish is served by SnapMap: nothing retained to certify here, so
             * ordinary resolution decides whether it is still a use at all. */
            if (!_stricmp(binding->name, binding->original) && !pr_provider_names(lease->provider, binding)) return 0;
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
    sh_package_fsm_webs *selected_webs = NULL, *webs = NULL;
    pr_retained_selection selected = {0};
    size_t *owners = NULL, *files = NULL;
    size_t old_owners = view->sources->package_count, old_files = view->sources->file_count;
    size_t maximum = view->compiled->resource_count + provider->compiled->resource_count, count = 0, next_owner = old_owners;
    int ok = 0, deferred_shared = 0;
    if (maximum < view->compiled->resource_count || maximum > SIZE_MAX / (2u * sizeof(*inputs)) ||
        provider->sources->package_count > SIZE_MAX / sizeof(*owners) ||
        provider->sources->file_count > SIZE_MAX / sizeof(*files)) goto done;
    if (!pr_retained_select(provider, admitted, mode, &selected, error, capacity)) goto done;
    pr_lease_fsm_declarations(&selected);
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
    if (provider->fsm_webs) {
        /* The view's own webs keep their behaviors (map before local). */
        selected_webs = sh_package_fsm_webs_filter(provider->fsm_webs, pr_lease_behavior, &selected, error, capacity);
        if (!selected_webs || !(webs = sh_package_fsm_webs_join(view->fsm_webs, selected_webs, error, capacity))) goto done;
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
    if (!pr_key_groups_add(view, provider, error, capacity) ||
        !pr_retained_bindings(view, provider, compiled, bundles, sources->package_count, error, capacity)) goto done;
    sh_package_compilation_free(view->compiled); view->compiled = compiled; compiled = NULL;
    sh_package_sources_free(view->sources); view->sources = sources; sources = NULL;
    sh_package_sources_free(view->authored_sources); view->authored_sources = authored; authored = NULL;
    for (size_t i = 0; i < view->bundle_count; i++) sh_package_bundle_release(view->bundles[i]);
    free(view->bundles); view->bundles = bundles; bundles = NULL; view->bundle_count = view->sources->package_count;
    if (materials) { sh_virtual_material_runtime_retire(&view->virtual_materials); view->virtual_materials = materials; materials = NULL; }
    if (consumers) { sh_package_consumers_free(view->consumers); view->consumers = consumers; consumers = NULL; }
    if (webs) { sh_package_fsm_webs_free(view->fsm_webs); view->fsm_webs = webs; webs = NULL; }
    ok = 1;
done:
    if (remapped) for (size_t i = 0; i < provider->compiled->resource_count; i++) {
        free(remapped[i].sources); sh_package_owners_free(&remapped[i].owners); sh_package_owners_free(&remapped[i].gameplay_owners);
    }
    if (bundles) { for (size_t i = 0; sources && i < sources->package_count; i++) sh_package_bundle_release(bundles[i]); free(bundles); }
    sh_virtual_material_runtime_retire(&filtered); sh_virtual_material_runtime_retire(&materials);
    sh_package_consumers_free(selected_consumers); sh_package_consumers_free(consumers);
    sh_package_fsm_webs_free(selected_webs); sh_package_fsm_webs_free(webs);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources); sh_package_sources_free(authored);
    sh_package_sources_free(extra); free(owners); free(files); free(projections); free(inputs); free(remapped);
    free(selected.resources); sh_package_owners_free(&selected.owners);
    if (!ok && error && capacity && !*error) snprintf(error, capacity, "cannot preserve immutable occurrence resource contexts");
    return ok;
}

typedef struct pr_spawn_table {
    const char *path;
    const sh_compiled_resource **rows;
    size_t count;
    unsigned char *body;
    size_t length;
} pr_spawn_table;
static int pr_unmodified_type(pr_provider *view, const sh_compiled_resource *resource, char *error, size_t capacity)
{
    size_t length = 0;
    unsigned char *body = sh_package_compilation_read(view->compiled, resource, SIZE_MAX, &length, error, capacity);
    sh_package_spawn_registry program = {0};
    sh_package_key_space numeric = {0}; /* Lowered classifier rows have numeric keys. */
    int status = body ? sh_package_spawn_registry_read((sh_decl_source){(char *)body, length}, &numeric,
        &program, error, capacity) : -1;
    free(body);
    int ok = status == 1 && program.count == 1;
    if (ok && !program.modifier_count && (program.rows[0].mask & SH_PACKAGE_KEY_PRIVATE)) {
        uint32_t type = program.rows[0].mask;
        size_t i;
        for (i = 0; i < view->unmodified_count; i++) if (view->unmodified_types[i] == type) break;
        if (i == view->unmodified_count) {
            uint32_t *grown = i < SIZE_MAX / sizeof(*grown) - 1 ?
                realloc(view->unmodified_types, (i + 1) * sizeof(*grown)) : NULL;
            if (!grown) ok = 0;
            else { view->unmodified_types = grown; view->unmodified_types[view->unmodified_count++] = type; }
        }
    }
    sh_package_spawn_registry_free(&program);
    if (!ok && error && capacity && !*error) snprintf(error, capacity, "cannot retain native classification modifier policy");
    return ok;
}

static const sh_compiled_resource *pr_spawn_resource(pr_provider *view,
    const sh_package_consumer_target *target, char *error, size_t capacity)
{
    if (strcmp(target->family, "entitydef")) return NULL;
    size_t length = strlen(target->name) + sizeof("generated/decls/entitydef/.decl");
    char *path = malloc(length);
    if (!path) { snprintf(error, capacity, "cannot allocate spawn classification path"); return NULL; }
    snprintf(path, length, "generated/decls/entitydef/%s.decl", target->name);
    const sh_compiled_resource *resource = sh_package_compilation_find(view->compiled, path);
    if (!resource || !resource->original_path) {
        snprintf(error, capacity, "%s: retained spawn classification is unavailable", path); resource = NULL;
    }
    free(path); return resource;
}

/* Dispatch metadata follows the final retained dependency closure, even during
 * editor-only additive admission where existing native tables stay untouched. */
static int pr_spawn_classifications(pr_provider *view, char *error, size_t capacity)
{
    view->unmodified_count = 0;
    for (size_t i = 0; i < sh_package_consumers_count(view->consumers); i++) {
        const sh_package_consumer *owner = sh_package_consumers_at(view->consumers, i);
        for (size_t j = 0; j < owner->count; j++) {
            const sh_package_consumer_target *target = owner->targets + j;
            if (strncmp(target->slot, "spawn.classifier:", 17)) continue;
            const sh_compiled_resource *resource = pr_spawn_resource(view, target, error, capacity);
            if (!resource || !pr_unmodified_type(view, resource, error, capacity)) return 0;
        }
    }
    return 1;
}

/* Native spawn requests carry only a numeric key. Their captured private
 * registry files stay in the ordinary retained dependency closure; publish
 * those files' rows into the original conductor slot every active view serves.
 * This projection never rewrites the immutable private files or source bundles. */
static int pr_spawn_registrations(pr_provider *view, char *error, size_t capacity)
{
    pr_spawn_table *tables = NULL;
    size_t count = 0;
    sh_package_resource_projection *projections = NULL;
    const sh_compiled_resource **inputs = NULL;
    sh_package_compilation *compiled = NULL;
    int ok = 0;
    for (size_t i = 0; i < sh_package_consumers_count(view->consumers); i++) {
        const sh_package_consumer *owner = sh_package_consumers_at(view->consumers, i);
        for (size_t j = 0; j < owner->count; j++) {
            const sh_package_consumer_target *target = owner->targets + j;
            if (strncmp(target->slot, "spawn.registry:", 15) && strncmp(target->slot, "spawn.classifier:", 17)) continue;
            const sh_compiled_resource *resource = pr_spawn_resource(view, target, error, capacity);
            if (!resource) goto done;
            size_t t;
            for (t = 0; t < count; t++) if (!strcmp(tables[t].path, resource->original_path)) break;
            if (t == count) {
                if (count >= SIZE_MAX / sizeof(*tables) - 1) goto done;
                pr_spawn_table *grown = realloc(tables, (count + 1) * sizeof(*grown));
                if (!grown) goto done;
                tables = grown; tables[count++] = (pr_spawn_table){resource->original_path};
            }
            pr_spawn_table *table = tables + t;
            size_t r;
            for (r = 1; r < table->count; r++) if (table->rows[r] == resource) break;
            if (r < table->count) continue;
            if (!table->count) {
                table->rows = calloc(2, sizeof(*table->rows));
                if (!table->rows) goto done;
                table->rows[0] = sh_package_compilation_find(view->compiled, table->path);
                if (!table->rows[0]) table->rows[0] = resource;
                table->count = 1;
            }
            if (table->count >= SIZE_MAX / sizeof(*table->rows) - 1) goto done;
            const sh_compiled_resource **grown = realloc(table->rows, (table->count + 1) * sizeof(*grown));
            if (!grown) goto done;
            table->rows = grown; table->rows[table->count++] = resource;
        }
    }
    if (!count) { ok = 1; goto done; }
    size_t maximum = view->compiled->resource_count + count;
    if (maximum < count || maximum > SIZE_MAX / sizeof(*projections) || maximum > SIZE_MAX / sizeof(*inputs)) goto done;
    projections = calloc(maximum, sizeof(*projections)); inputs = calloc(maximum, sizeof(*inputs));
    if (!projections || !inputs) goto done;
    size_t total = view->compiled->resource_count;
    for (size_t i = 0; i < total; i++) {
        const sh_compiled_resource *r = view->compiled->resources + i;
        inputs[i] = r;
        projections[i] = (sh_package_resource_projection){r->engine_path, inputs + i, 1,
            NULL, 0, r->native_type, r->native_name};
    }
    for (size_t t = 0; t < count; t++) {
        pr_spawn_table *table = tables + t;
        sh_decl_source *texts = calloc(table->count, sizeof(*texts));
        if (!texts) goto done;
        int read = 1;
        for (size_t r = 0; r < table->count; r++) {
            texts[r].text = (char *)sh_package_compilation_read(view->compiled, table->rows[r], SIZE_MAX,
                &texts[r].length, error, capacity);
            if (!texts[r].text) { read = 0; break; }
        }
        if (read) table->body = (unsigned char *)sh_package_spawn_registry_compose(texts[0], texts + 1,
            table->count - 1, view->key_groups, &table->length, error, capacity);
        for (size_t r = 0; r < table->count; r++) free((char *)texts[r].text);
        free(texts); if (!table->body) goto done;
        const sh_compiled_resource *base = sh_package_compilation_find(view->compiled, table->path);
        size_t at = base ? (size_t)(base - view->compiled->resources) : total++;
        projections[at] = (sh_package_resource_projection){table->path, table->rows, table->count,
            table->body, table->length};
    }
    compiled = sh_package_compilation_project(view->sources, projections, total, &view->compiled->policy,
        view->compiled->canonical_path, error, capacity);
    if (!compiled) goto done;
    compiled->independent_sources = view->compiled->independent_sources;
    compiled->map_overlay = view->compiled->map_overlay; compiled->map_owner_begin = view->compiled->map_owner_begin;
    compiled->map_independent_sources = view->compiled->map_independent_sources;
    sh_package_compilation_free(view->compiled); view->compiled = compiled; compiled = NULL; ok = 1;
done:
    for (size_t t = 0; t < count; t++) { free(tables[t].rows); free(tables[t].body); }
    free(tables); free(projections); free(inputs); sh_package_compilation_free(compiled);
    if (!ok && error && capacity && !*error) snprintf(error, capacity, "cannot retain native spawn registry bindings");
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
    if (!pr_key_groups_add(view, library, error, capacity) || !pr_key_groups_add(view, map, error, capacity)) {
        pr_release(view); return NULL;
    }
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
    if ((library->fsm_webs || (map && map->fsm_webs)) && !(view->fsm_webs =
        sh_package_fsm_webs_join(map ? map->fsm_webs : NULL, library->fsm_webs, error, capacity))) { pr_release(view); return NULL; }
#ifndef SH_PACKAGE_RUNTIME_TESTING
    /* Served from the first view on, whatever the packages bind: an issued
     * declaration stays excluded for the life of the process, and its
     * exclusion needs the placeholder registered and non-selecting. */
    if (library->catalog) {
        unsigned char *placeholder = NULL;
        size_t length = 0;
        int status = pr_fsm_placeholder_read(sh_resource_catalog_read_path, library->catalog, &placeholder, &length);
        int added = status == 3 && sh_package_compilation_add_declaration(view->compiled, "aifsmmanager",
            SH_PACKAGE_FSM_PLACEHOLDER, placeholder, length, 3, error, capacity);
        free(placeholder);
        /* Without its installed source no web can bind (pr_fsm_bind refuses
         * views that have any); views without webs still serve. */
        if (!added && status == 3) { pr_release(view); return NULL; }
        if (!added) backend_log("AI state-machine placeholder unavailable: " SH_PACKAGE_FSM_PLACEHOLDER_SOURCE
            " could not be read; package state-machine webs cannot bind");
    }
#endif
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
    if (!pr_spawn_registrations(view, error, capacity) || !pr_prepare_spawn_program(view, error, capacity)) {
        pr_release(view); return NULL;
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
/* A library an additive increment extends reports what a rescan of the same
 * sources would: the increment's own scan with the packages it left out again,
 * and the accounted coverage of everything the result serves. */
static int pr_carry_report(pr_provider *library, const pr_provider *base, const pr_provider *increment)
{
    size_t a = base->coverage ? strlen(base->coverage) : 0, b = increment->coverage ? strlen(increment->coverage) : 0;
    if ((increment->rejections && !(library->rejections = _strdup(increment->rejections))) ||
        (increment->unserved && !(library->unserved = _strdup(increment->unserved)))) return 0;
    if (a || b) {
        if (a > SIZE_MAX - b - 1 || !(library->coverage = malloc(a + b + 1))) return 0;
        if (a) memcpy(library->coverage, base->coverage, a);
        if (b) memcpy(library->coverage + a, increment->coverage, b);
        library->coverage[a + b] = 0;
    }
    for (size_t i = 0; i < increment->exclusion_count; i++) {
        const pr_exclusion *item = increment->exclusions + i;
        if (!pr_exclusion_add(&library->exclusions, &library->exclusion_count, item->name, item->tree,
            item->line)) return 0;
    }
    return 1;
}

static pr_provider *pr_clone_serving(pr_provider *base, char *error, size_t capacity);
/* base extended by an increment's newly compiled packages; every unchanged
 * package is served from base itself (sh_package_map_plan_prepare_inventory,
 * pr_refresh_increment). The own-bytes memo then holds the library's complete
 * list, so a package an increment left out stays out at the next start. */
static pr_provider *pr_increment_view(pr_provider *base, const pr_provider *increment,
    char *error, size_t capacity)
{
    int metadata_needed = 0;
    pr_provider *library = pr_clone_serving(base, error, capacity);
    if (!library || !pr_view_retained(library, increment, library->sources->package_count,
        NULL, NULL, PR_RETAIN_AUTHORING, &metadata_needed, error, capacity) ||
        !pr_prepare_spawn_program(library, error, capacity)) { pr_release(library); return NULL; }
    if (!pr_carry_report(library, base, increment)) {
        if (error && capacity) snprintf(error, capacity, "cannot retain the installed library report");
        pr_release(library); return NULL;
    }
    if (library->data_root) pr_exclusions_store(library->data_root, library->exclusions, library->exclusion_count);
    return library;
}
/* An installed-library refresh (a rearm, or the rescan after a committed
 * installation) whose scan only adds packages to base: compile the added
 * packages alone and serve the rest from base, as an installation does.
 * 1 with the new library and inventory; 0 when a package was removed or
 * changed or the increment did not complete (an environment failure, or a
 * meaning the additive view cannot admit), and the caller compiles the whole
 * library, which judges and attributes every failure itself. */
static int pr_refresh_increment(pr_provider *base, const pr_provider *string_peer,
    pr_provider **library, pr_inventory **inventory, char *error, size_t capacity)
{
    int composition_failed = 0;
    *library = NULL; *inventory = NULL;
    if (!base || !base->data_root) return 0;
    pr_provider *added = pr_prepare_deliveries(base->data_root, NULL, base, 0, PR_INCREMENT_REFRESH, inventory,
        &composition_failed, NULL, NULL, 0, 0, string_peer, NULL, 0, error, capacity);
    if (added) *library = pr_increment_view(base, added, error, capacity);
    pr_release(added);
    if (*library) return 1;
    pr_inventory_release(*inventory); *inventory = NULL;
    return 0;
}
static pr_provider *pr_clone_serving(pr_provider *base, char *error, size_t capacity)
{
    sh_package_sources empty = {0};
    sh_package_resource_projection *rows = NULL;
    const sh_compiled_resource **inputs = NULL;
    pr_provider *view = calloc(1, sizeof(*view));
    if (!view) return NULL;
    view->references = 1; view->parent = pr_retain(base);
    if (!pr_key_groups_add(view, base, error, capacity)) goto failed;
    if (base->unmodified_count) {
        view->unmodified_types = malloc(base->unmodified_count * sizeof(*view->unmodified_types));
        if (!view->unmodified_types) goto failed;
        memcpy(view->unmodified_types, base->unmodified_types, base->unmodified_count * sizeof(*view->unmodified_types));
        view->unmodified_count = base->unmodified_count;
    }
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
    if (base->fsm_webs && !(view->fsm_webs = sh_package_fsm_webs_join(base->fsm_webs, NULL, error, capacity))) goto failed;
    if (!pr_prepare_spawn_program(view, error, capacity)) goto failed;
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
/* Alias membership is capture metadata, not an engine redirect. A stock name
 * can become known through another scope's shared input without changing any
 * byte it resolves. Omit that availability delta only when EVERY carrier is
 * unchanged and the installed SnapMap catalog proves this exact name/path
 * association. Keep private names, changed inputs and unknown associations.
 * Constructor changes are checked separately after this classification. */
static int pr_membership_changes(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, const sh_package_changes *changes,
    sh_package_native_identity *identities, size_t *count)
{
    if (!*count || !catalog || !compiled) return 1;
    unsigned char *state = calloc(*count, 1);
    if (!state) return 0;
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = compiled->resources + i;
        sh_package_change key = {resource->engine_path, 0};
        int changed = changes->count && bsearch(&key, changes->items, changes->count,
            sizeof(key), pr_change_order) != NULL;
        for (size_t n = 0; n < sh_package_resource_native_count(resource); n++) {
            sh_package_native_identity identity = sh_package_resource_native_at(resource, n);
            sh_package_native_identity *found = bsearch(&identity, identities, *count,
                sizeof(identity), pr_identity_order);
            if (!found || state[found - identities] == 2) continue;
            state[found - identities] = changed || !pr_snapmap_serves((void *)catalog,
                resource->engine_path, identity.type, identity.name) ? 2 : 1;
        }
    }
    size_t kept = 0;
    for (size_t i = 0; i < *count; i++) {
        if (state[i] == 1) { free(identities[i].type); free(identities[i].name); }
        else identities[kept++] = identities[i];
    }
    free(state); *count = kept; return 1;
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
    if (!pr_membership_changes(before ? before->compiled : NULL, before ? before->catalog : NULL,
            changes, changes->removed, &changes->removed_count) ||
        !pr_membership_changes(after->compiled, after->catalog,
            changes, changes->added, &changes->added_count)) goto failed;
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

#ifdef SH_PACKAGE_RUNTIME_TESTING
int sh_package_runtime_test_native_changes(const sh_package_compilation *before,
    const sh_package_compilation *after, const sh_resource_catalog *catalog,
    sh_package_consumers *old_consumers, sh_package_consumers *new_consumers,
    sh_package_changes *changes, char *error, size_t capacity)
{
    pr_provider a = {0}, b = {0};
    a.compiled = (sh_package_compilation *)before; a.consumers = old_consumers;
    b.compiled = (sh_package_compilation *)after; b.consumers = new_consumers;
    a.catalog = b.catalog = (sh_resource_catalog *)catalog;
    return sh_package_compilation_changes(before, after, changes, error, capacity) &&
        pr_native_changes(&a, &b, changes, error, capacity);
}
#endif

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
        int installed = InterlockedExchange(&g_install_published, 0) != 0;
        if (installed && g_library_provider && pr_refresh_increment(g_library_provider, g_map_provider,
                &library, &inventory, error, sizeof(error)))
            backend_log("package library refresh for a published installation: added packages compiled alone, "
                "unchanged packages served from the retained library");
        else {
            if (installed && g_library_provider) {
                snprintf(line, sizeof(line), "package library refresh for a published installation compiles the whole library: %.400s",
                    error[0] ? error : "the increment did not complete");
                backend_log(line);
            }
            error[0] = 0;
            library = pr_prepare(data_root, NULL, g_library_provider, 0, &inventory,
                &composition_failed, NULL, g_map_provider, error, sizeof(error));
        }
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
    if (activate && !pr_fsm_issue(current, error, sizeof(error))) goto done;
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
    if (activate && (!pr_activate(activate, context, 0, &changes, error, sizeof(error)) ||
        !pr_fsm_bind(current, error, sizeof(error)))) {
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
        recovered = pr_activate(activate, context, 1, &changes, recovery_error, sizeof(recovery_error)) &&
            pr_fsm_bind(old_current, recovery_error, sizeof(recovery_error));
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
    if (!pr_prepare_spawn_program(candidate, error, capacity)) goto done;
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
    if (!pr_fsm_issue(candidate, error, capacity)) goto done;
    AcquireSRWLockExclusive(&g_lock);
    previous = g_current_provider; old_ready = g_ready; old_usable = g_usable;
    if (!sh_virtual_material_runtime_publish(candidate->virtual_materials, &previous_materials, error, capacity)) {
        ReleaseSRWLockExclusive(&g_lock); goto done;
    }
    pr_publish(candidate, g_library_provider, g_map_provider);
    g_activating = 1; g_ready = g_usable = 0;
    ReleaseSRWLockExclusive(&g_lock);
    if (guarded) { guard.end(1); guarded = 0; }
    int native_ready = pr_activate(activate, context, 0, &changes, error, capacity) && pr_fsm_bind(candidate, error, capacity);
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
        int recovered = pr_activate(activate, context, 1, &changes, recovery_error, sizeof(recovery_error)) &&
            pr_fsm_bind(previous, recovery_error, sizeof(recovery_error));
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
        char error[1024] = "";
        /* The restored provider's state-machine table binds again first. */
        if (!pr_fsm_bind(g_occurrence_recovery_provider, error, sizeof(error))) {
            snprintf(g_error, sizeof(g_error), "occurrence recovery could not restore AI state-machine webs: %s", error);
        } else {
            retained = g_occurrence_recovery_provider; g_occurrence_recovery_provider = NULL;
            g_occurrence_recovery_context = NULL;
            g_ready = g_occurrence_recovery_ready; g_usable = g_occurrence_recovery_usable; g_error[0] = 0;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    pr_release(retained); ReleaseSRWLockExclusive(&g_refresh_lock); return retained != NULL;
}

static sh_package_map_plan *pr_prepare_map(const char *data_root,
    const char *map_source_root, const sh_package_provenance *contexts,
    const sh_package_bundle_binding *deliveries, size_t delivery_count, int use_deliveries,
    const char *document, size_t document_length, char *error, size_t capacity)
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
        0, 0, NULL, NULL, contexts, deliveries, delivery_count, use_deliveries, g_library_provider,
        document, document_length, error, capacity);
    if (!plan->provider) { free(plan); plan = NULL; }
done:
    ReleaseSRWLockExclusive(&g_refresh_lock); return plan;
}

sh_package_map_plan *sh_package_runtime_prepare_map_with_bindings(const char *data_root,
    const char *map_source_root, const sh_package_provenance *contexts, char *error, size_t capacity)
{ return pr_prepare_map(data_root, map_source_root, contexts, NULL, 0, 0, NULL, 0, error, capacity); }

sh_package_map_plan *sh_package_runtime_prepare_map_deliveries(const char *data_root,
    const char *map_source_root, const sh_package_provenance *contexts,
    const sh_package_bundle_binding *deliveries, size_t delivery_count, char *error, size_t capacity)
{ return pr_prepare_map(data_root, map_source_root, contexts, deliveries, delivery_count, 1, NULL, 0, error, capacity); }

sh_package_map_plan *sh_package_runtime_prepare_map_document(const char *data_root,
    const char *map_source_root, const sh_package_provenance *contexts,
    const sh_package_bundle_binding *deliveries, size_t delivery_count,
    const char *json, size_t length, char *error, size_t capacity)
{
    return pr_prepare_map(data_root, map_source_root, contexts, deliveries, delivery_count, 1,
        contexts ? NULL : json, length, error, capacity);
}

sh_package_map_plan *sh_package_runtime_prepare_map(const char *data_root,
    const char *map_source_root, char *error, size_t capacity)
{ return sh_package_runtime_prepare_map_with_bindings(data_root, map_source_root, NULL, error, capacity); }

typedef struct pr_local_binding {
    const sh_resource_catalog *catalog;
    pr_original_source originals;
} pr_local_binding;
static int pr_local_native(void *context, const char *family, const char *name)
{
    pr_local_binding *local = context;
    const sh_resource_catalog_entry *const *entries = NULL;
    size_t count = sh_resource_catalog_find(local->catalog, family, name, &entries);
    for (size_t i = 0; i < count; i++) if (entries[i]->archive < 2) return 1;
    size_t a = strlen(family), b = strlen(name), length = 0;
    unsigned char *body = NULL; char *path;
    if (a > SIZE_MAX - 23 || b > SIZE_MAX - a - 23) return -1;
    path = malloc(a + b + 23); if (!path) return -1;
    snprintf(path, a + b + 23, "generated/decls/%s/%s.decl", family, name);
    int scope = pr_original(&local->originals, path, &body, &length);
    free(path); free(body);
    return scope < 0 ? -1 : scope == 1 || scope == 3;
}
char *sh_package_runtime_rebind_local_document(const char *json, size_t length,
    size_t *output_length, char *error, size_t capacity)
{
    char *bound = NULL, *out = NULL; size_t bound_length = length;
    sh_package_source_reader *reader = NULL;
    sh_decl_registry_source registry = {0}; uintptr_t reflection = 0;
    if (error && capacity) *error = 0;
    if (output_length) *output_length = 0;
    if (!json || !output_length || length == SIZE_MAX) return NULL;
    AcquireSRWLockExclusive(&g_refresh_lock);
    AcquireSRWLockShared(&g_lock);
    const pr_provider *library = g_library_provider;
    if (library && library->bindings.binding_count) {
        pr_local_binding local = {library->catalog,
            {library->catalog ? sh_resource_catalog_read_path : NULL, library->catalog, library->native_assets}};
#ifndef SH_PACKAGE_RUNTIME_TESTING
        sh_decl_native_source metadata = {NULL, pr_native_read, 0, g_native_scripts};
        if (!sh_decl_server_registry_source(&registry) || sh_decl_native_source_ready(&metadata,
            g_native_accessor, g_native_types, g_native_game_system) != 1) goto done;
        reflection = metadata.reflection;
#else
        if (!sh_package_runtime_test_source_inputs(&registry, &reflection,
            &local.originals.game, &local.originals.context)) goto done;
#endif
        reader = sh_package_source_reader_open(library->compiled, library->catalog,
            pr_original, &local.originals, registry, reflection, error, capacity);
        if (!reader) goto done;
        bound = sh_package_map_uses_bind_local(reader, json, length, &library->bindings,
            pr_local_native, &local, &bound_length, error, capacity);
        if (!bound) goto done;
    }
    if (bound_length == SIZE_MAX) goto done;
    out = HeapAlloc(GetProcessHeap(), 0, bound_length + 1);
    if (out) {
        memcpy(out, bound ? bound : json, bound_length); out[bound_length] = 0;
        *output_length = bound_length;
    }
done:
    sh_package_source_reader_close(reader); free(bound);
    ReleaseSRWLockShared(&g_lock); ReleaseSRWLockExclusive(&g_refresh_lock);
    if (!out && error && capacity && !*error)
        snprintf(error, capacity, "local legacy resource names could not be prepared");
    return out;
}

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
    int ok = 0;
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
    checked = pr_prepare_deliveries(data_root, NULL, base, 0, PR_INCREMENT_INSTALL,
        &inventory, &composition_failed, NULL, NULL, 0, 0, plan->provider, NULL, 0, error, capacity);
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
    if (checked && !(library = pr_increment_view(base, checked, error, capacity))) goto done;
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

/* A growable summary: each line is sized from what it prints, so no package,
 * component or diagnostic count can truncate it. failed is sticky. */
typedef struct pr_summary { char *text; size_t length, capacity; int failed; } pr_summary;
static void pr_summary_put(pr_summary *s, const char *format, ...)
{
    va_list args;
    int needed;
    if (s->failed) return;
    va_start(args, format); needed = vsnprintf(NULL, 0, format, args); va_end(args);
    if (needed < 0) { s->failed = 1; return; }
    if ((size_t)needed >= s->capacity - s->length) {
        size_t next = s->capacity ? s->capacity : 4096;
        while ((size_t)needed >= next - s->length) {
            if (next > SIZE_MAX / 2) { s->failed = 1; return; }
            next *= 2;
        }
        char *grown = (char *)realloc(s->text, next);
        if (!grown) { s->failed = 1; return; }
        s->text = grown; s->capacity = next;
    }
    va_start(args, format); vsnprintf(s->text + s->length, s->capacity - s->length, format, args); va_end(args);
    s->length += (size_t)needed;
}
/* The game settings the active compilation requires (policy requirements):
 * each composed value, then every component requesting it with its own value
 * (a package's nested components may ask differently from its root). Called
 * with g_lock held. */
static void pr_summary_settings(pr_summary *s)
{
    sh_json_object cvars = {0};
    const char *raw = sh_json_object_get(&g_compiled->policy.requirements, "cvars");
    if (!raw || !sh_json_parse_object(raw, strlen(raw), 2, &cvars) || !cvars.count) {
        pr_summary_put(s, "Effective settings: no package requires a game setting.\n");
        sh_json_object_free(&cvars); return;
    }
    pr_summary_put(s, "Effective settings required by packages:\n");
    for (size_t c = 0; c < cvars.count; c++) {
        size_t listed = 0;
        pr_summary_put(s, "  %s = %s; requested by", cvars.members[c].key, cvars.members[c].value_json);
        for (size_t i = 0; i < g_sources->package_count; i++) {
            const sh_package_descriptor *root = NULL;
            for (size_t j = 0; j < g_sources->component_count && !root; j++)
                if (g_sources->components[j].owner == i && !g_sources->components[j].relative[0])
                    root = &g_sources->components[j].descriptor;
            for (size_t j = 0; j < g_sources->component_count; j++) {
                const sh_package_component *component = g_sources->components + j;
                sh_json_object requirements = {0}, own = {0};
                const char *section, *value;
                if (component->owner != i) continue;
                if ((section = sh_json_object_get(&component->descriptor.fields, "requirements")) &&
                    sh_json_parse_object(section, strlen(section), 3, &requirements) &&
                    (section = sh_json_object_get(&requirements, "cvars")) &&
                    sh_json_parse_object(section, strlen(section), 2, &own) &&
                    (value = sh_json_object_get(&own, cvars.members[c].key)) != NULL) {
                    if (component->relative[0])
                        pr_summary_put(s, "%s %s [%s] component %s (%s)", listed++ ? "," : "",
                            root ? root->name : "Package", root ? root->id : "?", component->relative, value);
                    else
                        pr_summary_put(s, "%s %s [%s] (%s)", listed++ ? "," : "", root ? root->name : "Package",
                            root ? root->id : "?", value);
                }
                sh_json_object_free(&own); sh_json_object_free(&requirements);
            }
        }
        if (!listed) pr_summary_put(s, " the active map's packages");
        pr_summary_put(s, "\n");
    }
    sh_json_object_free(&cvars);
}
char *sh_package_runtime_summary(void)
{
    pr_summary s = {0};
    size_t i, j;
    AcquireSRWLockShared(&g_lock);
    const char *rejections = g_library_provider ? g_library_provider->rejections : NULL;
    const char *coverage = g_library_provider ? g_library_provider->coverage : NULL;
    const char *unserved = g_library_provider ? g_library_provider->unserved : NULL;
    pr_summary_put(&s, "Installed library: %zu packages.\n", g_inventory ? g_inventory->sources->package_count : 0);
    if (rejections) pr_summary_put(&s, "%s%s", rejections,
        "Skipped packages stay unchanged on disk. Packages made for earlier Snapmap+ releases can be "
        "converted: close DOOM and run snapmap-plus migrate-overrides.\n");
    if (unserved) pr_summary_put(&s, "%s%s", unserved,
        "Packages not served yet stay installed and unchanged; they need a Snapmap+ update, not an edit.\n");
    if (coverage) pr_summary_put(&s, "%s", coverage);
    if (g_authoring_error[0]) pr_summary_put(&s,
        "Local authoring composition unavailable: %s\nIndependent map packages can still compile.\n", g_authoring_error);
    if (!g_ready) pr_summary_put(&s, "Packages could not compile: %s\n%s",
        g_error[0] ? g_error : "compiler has not initialized",
        g_compiled ? "The last successful compilation remains active.\n" : "");
    if (!g_sources || !g_compiled || !g_sources->package_count) {
        pr_summary_put(&s, "No packages in the active compilation.\n"); goto done;
    }
    pr_summary_put(&s, "%zu packages; %zu native resources; %zu identical duplicates; %zu composed declarations.\n",
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
        pr_summary_put(&s, "%s [%s]\n  %zu files, %llu bytes, %zu components; %s\n  %s\n",
            descriptor ? descriptor->name : "Package", descriptor ? descriptor->id : "?",
            files, (unsigned long long)bytes, components, gameplay ? "contains gameplay resources" : "editor support",
            g_sources->packages[i].root);
    }
    pr_summary_settings(&s);
done:
    ReleaseSRWLockShared(&g_lock);
    if (s.failed) { free(s.text); return NULL; }
    return s.text;
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
        *body = sh_package_compilation_read_served(g_compiled, resource, (size_t)PTRDIFF_MAX,
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
        *body = sh_package_compilation_read_served(g_compiled, resource, (size_t)INT_MAX, length, error, sizeof(error));
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
    keys->other = owner && owner != g_current_provider && g_current_provider ? g_current_provider->key_groups : NULL;
    keys->scratch = owner ? calloc(1, sizeof(*keys->scratch)) : NULL;
    if (keys->scratch && owner->key_groups) *keys->scratch = *owner->key_groups;
    pr_key_taken(g_current_provider, &keys->taken);
    pr_key_taken(owner, &keys->taken);
}
/* Adopt new group codes a capture defined, and publish them (exclusive). */
static int pr_key_context_close(pr_key_context *keys, int adopt)
{
    int ok = !adopt || !keys->owner || keys->scratch != NULL;
    if (adopt && keys->scratch && keys->owner &&
        keys->scratch->count > (keys->owner->key_groups ? keys->owner->key_groups->count : 0)) {
        AcquireSRWLockExclusive(&g_lock);
        sh_package_key_groups *serving = NULL;
#ifdef PR_NATIVE_SPAWN
        sh_spawn_keys_program *program = NULL;
#endif
        int publish = keys->owner == g_current_provider ||
            keys->owner == g_library_provider || keys->owner == g_map_provider;
        if (publish && keys->owner != g_current_provider && g_current_provider) {
            serving = calloc(1, sizeof(*serving));
            if (serving && g_current_provider->key_groups) *serving = *g_current_provider->key_groups;
            ok = serving && sh_package_key_groups_merge(serving, keys->scratch);
        }
#ifdef PR_NATIVE_SPAWN
        if (ok && publish && g_current_provider) {
            program = sh_spawn_keys_program_create(keys->owner == g_current_provider ? keys->scratch : serving,
                g_current_provider->unmodified_types, g_current_provider->unmodified_count);
            ok = program != NULL;
        }
#endif
        /* Prepare both dictionaries before transferring either pointer. A
         * failed allocation must not commit a code only one consumer knows. */
        if (ok) {
            sh_package_key_groups *old = keys->owner->key_groups;
            keys->owner->key_groups = keys->scratch; keys->scratch = old;
            if (serving) {
                old = g_current_provider->key_groups;
                g_current_provider->key_groups = serving; serving = old;
            }
#ifdef PR_NATIVE_SPAWN
            if (program) {
                sh_spawn_keys_program *old_program = g_current_provider->spawn_program;
                g_current_provider->spawn_program = program; program = old_program;
            }
#endif
            if (publish) pr_publish_key_groups();
        }
#ifdef PR_NATIVE_SPAWN
        sh_spawn_keys_program_release(program);
#endif
        free(serving);
        ReleaseSRWLockExclusive(&g_lock);
    }
    free(keys->scratch); keys->scratch = NULL;
    return ok;
}

void sh_package_runtime_key_decode(uint32_t value, sh_package_key_set *keys, uint32_t *historical)
{
    AcquireSRWLockShared(&g_lock);
    pr_provider *owner = g_current_provider;
    const sh_package_key_groups *groups = owner ? owner->key_groups : NULL;
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
    if (!pr_key_context_close(&context, result == 1)) { *code = 0; result = -1; }
    ReleaseSRWLockExclusive(&g_refresh_lock);
    return result;
}
static char *pr_capture_document(const pr_provider *provider,
    sh_decl_registry_source registry, uintptr_t reflection, int retain_native,
    const sh_package_provenance *saved, const sh_package_bundle_binding *additional,
    size_t additional_count, const char *json, size_t length, size_t *output_length,
    sh_package_occurrence_snapshot **ownership, sh_mpkg_used **deliveries,
    size_t *delivery_count, sh_package_references *closure, pr_key_context *keys,
    char *error, size_t capacity)
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
    if (closure && deliveries) { sh_package_references_free(closure); *closure = resources; memset(&resources, 0, sizeof(resources)); }
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
    size_t pending_length = 0, pending_count = 0;
    sh_package_occurrence_snapshot *pending_ownership = NULL;
    sh_mpkg_used *pending_deliveries = NULL;
    if (error && capacity) *error = 0;
    if (output_length) *output_length = 0;
    if (!ownership || !output_length || (!!deliveries != !!delivery_count)) {
        if (error && capacity) snprintf(error, capacity, "map occurrence output is unavailable");
        return NULL;
    }
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
            saved, additional, additional_count, json, length, &pending_length, &pending_ownership,
            deliveries ? &pending_deliveries : NULL, deliveries ? &pending_count : NULL, NULL, &keys, error, capacity);
#else
    sh_decl_registry_source registry = {0}; uintptr_t reflection = 0;
    if (sh_package_runtime_test_source_inputs(&registry, &reflection, NULL, NULL))
        out = pr_capture_document(plan ? plan->provider : g_current_provider, registry, reflection, !plan,
            saved, additional, additional_count, json, length, &pending_length, &pending_ownership,
            deliveries ? &pending_deliveries : NULL, deliveries ? &pending_count : NULL, NULL, &keys, error, capacity);
#endif
    ReleaseSRWLockShared(&g_lock);
    if (!pr_key_context_close(&keys, out != NULL)) {
        free(out); out = NULL;
        if (error && capacity) snprintf(error, capacity, "map registration dictionary could not be published");
    }
    if (out) {
        sh_package_occurrence_release(*ownership); *ownership = pending_ownership; pending_ownership = NULL;
        if (deliveries) {
            sh_mpkg_used_free(*deliveries); *deliveries = pending_deliveries; pending_deliveries = NULL;
            *delivery_count = pending_count;
        }
        *output_length = pending_length;
    }
    sh_package_occurrence_release(pending_ownership); sh_mpkg_used_free(pending_deliveries);
    if (!g_activation_callback) ReleaseSRWLockExclusive(&g_refresh_lock);
    if (!out && error && capacity && !*error) snprintf(error, capacity, "native map occurrence reader is unavailable");
    return out;
}

/* ---- R137 save-time native representation ---- */

/* SnapMap's entityDef carrier writes this state version (VK FUN_1409d1990 /
 * GL FUN_1409d1150); other versions are upgraded natively before use. */
#define PR_ENTITY_STATE_VERSION 26

typedef struct pr_inline_kept {
    unsigned char tree[32];
    sh_decl_carrier_limit limit;
    char reason[320];
} pr_inline_kept;
typedef struct pr_inline_view {
    const pr_provider *provider;
    pr_original_source originals;
    sh_decl_native_schema *schema;
    const sh_package_provenance *uses;
    pr_inline_kept *kept;
    size_t kept_count;
    sh_editor_rows package_rows;
    int package_rows_state; /* 0 unread, 1 ready, -1 unavailable */
    char placement[320];
} pr_inline_view;

/* A copy the product serves under a native declaration's own name (the
 * installed library links a package's stock dependency closure that way, as a
 * canonical re-serialization) is that original for native representation only
 * when it parses to the same members in the same order, except where editor
 * composition moves members of its family's one block without changing them:
 * an entity definition's editorVars may be re-sorted, and an editor entry's
 * or composed inspector's collection fields may move to the tail of edit
 * (sh_package_editor_reordered_blocks, the rule the library uses to keep an
 * unchanged declaration's bytes). Other families compare strictly in order.
 * Anything else keeps the package route. 1 equal, 0 changed or not such a
 * copy, -1 unavailable. */
static int pr_stock_copy(const pr_original_source *originals, const sh_package_compilation *compiled,
    const sh_compiled_resource *resource)
{
    unsigned char *served = NULL, *original = NULL;
    size_t served_length = 0, original_length = 0;
    char error[256];
    int result = -1;
    if (!resource->baseline_known || (resource->original_path && _stricmp(resource->original_path, resource->engine_path)))
        return 0;
    served = sh_package_compilation_read(compiled, resource, SIZE_MAX / 2, &served_length, error, sizeof(error));
    int scope = served ? pr_original((void *)originals, resource->engine_path, &original, &original_length) : -1;
    if (scope <= 0) { result = scope < 0 ? -1 : 0; goto done; }
    sh_decl_node *left = sh_decl_tree_parse((sh_decl_source){(const char *)served, served_length}, error, sizeof(error));
    sh_decl_node *right = left ? sh_decl_tree_parse((sh_decl_source){(const char *)original, original_length}, error, sizeof(error)) : NULL;
    result = left && right ? sh_decl_tree_equal(left, right, sh_package_editor_reordered_blocks(resource->type)) : 0;
    sh_decl_tree_free(left); sh_decl_tree_free(right);
done:
    free(served); free(original);
    return result;
}
static int pr_inline_read(void *context, const char *name, sh_decl_source *text, int *served, char **replaces)
{
    pr_inline_view *view = context;
    size_t length = strlen(name) + sizeof("generated/decls/entitydef/.decl");
    char *path = malloc(length), error[256];
    unsigned char *body = NULL; size_t size = 0;
    if (!path) return -1;
    snprintf(path, length, "generated/decls/entitydef/%s.decl", name);
    const sh_compiled_resource *resource = sh_package_compilation_find(view->provider->compiled, path);
    int native_copy = resource && !resource->native_original && !resource->restored_original ?
        pr_stock_copy(&view->originals, view->provider->compiled, resource) : 0;
    if (native_copy < 0) { free(path); return -1; }
    if (resource && !resource->native_original && !resource->restored_original && !native_copy) {
        const char *original = resource->original_path;
        size_t original_length = original ? strlen(original) : 0;
        if (replaces && resource->baseline_known && original_length > 31 &&
            !strncmp(original, "generated/decls/entitydef/", 26) && !strcmp(original + original_length - 5, ".decl") &&
            _stricmp(original, path)) {
            if (!(*replaces = malloc(original_length - 30))) { free(path); return -1; }
            memcpy(*replaces, original + 26, original_length - 31); (*replaces)[original_length - 31] = 0;
        }
        body = sh_package_compilation_read(view->provider->compiled, resource, SIZE_MAX / 2, &size, error, sizeof(error));
        free(path);
        if (!body) { if (replaces) { free(*replaces); *replaces = NULL; } return -1; }
        *served = 1;
    } else {
        int scope = pr_original(&view->originals, path, &body, &size);
        free(path);
        if (scope < 0) { free(body); return -1; }
        if (!scope) { free(body); return 0; }
        *served = 0;
    }
    text->text = (const char *)body; text->length = size;
    return 1;
}
static int pr_inline_derives(void *context, const char *class_name, const char *base_name)
{
    pr_inline_view *view = context;
    return sh_decl_native_schema_class_derives(view->schema, class_name, base_name);
}
static const sh_package_provenance_use *pr_inline_site(const sh_package_provenance *uses, size_t entity)
{
    char owner[64];
    snprintf(owner, sizeof(owner), "entity/%zu", entity);
    for (size_t i = 0; uses && i < uses->use_count; i++) {
        const sh_package_provenance_use *use = uses->uses + i;
        if (use->owner && use->reference && !strcmp(use->owner, owner) &&
            !strcmp(use->reference, "/k:entityDef/k:inherit")) return use;
    }
    return NULL;
}
/* Another typed site bound to the same definition may select entities by it
 * (logic filters, targets). Other entities inheriting it are independent uses. */
static int pr_inline_looked_up(void *context, size_t entity, const char *name)
{
    pr_inline_view *view = context;
    const sh_package_provenance_use *site = pr_inline_site(view->uses, entity);
    (void)name;
    if (!site) return 0;
    for (size_t i = 0; i < view->uses->use_count; i++) {
        const sh_package_provenance_use *use = view->uses->uses + i;
        if (use == site || use->binding != site->binding) continue;
        if (use->owner && !strncmp(use->owner, "entity/", 7) && use->reference &&
            !strcmp(use->reference, "/k:entityDef/k:inherit")) continue;
        return 1;
    }
    return 0;
}
static const char *pr_inline_keep(void *context, size_t entity, const char *name, sh_decl_carrier_limit *limit)
{
    pr_inline_view *view = context;
    const sh_package_provenance_use *site = pr_inline_site(view->uses, entity);
    (void)name;
    *limit = SH_DECL_CARRIER_GAP;
    if (!site) return "this entity has no verified package use to replace";
    for (size_t c = 0; c < site->context_count; c++) {
        size_t index = site->contexts[c];
        if (index >= view->uses->context_count) return "this use has no recorded source context";
        const sh_package_provenance_context *source = view->uses->contexts + index;
        for (size_t t = 0; t < source->tree_count; t++)
            for (size_t k = 0; k < view->kept_count; k++)
                if (!memcmp(source->trees[t], view->kept[k].tree, 32)) {
                    *limit = view->kept[k].limit;
                    return view->kept[k].reason;
                }
    }
    return NULL;
}

/* ---- R137 native placement ----
 * A converted entity must pass SnapMap's map validation: its native base needs
 * a row among SnapMap's shipped editor declarations, and an editor row a
 * package adds for its definition must add nothing (package_editor_rows.h). */
#define PR_EDITOR_PREFIX "generated/decls/snapeditorentitydef/"
static int pr_name_order(const void *a, const void *b)
{ return strcmp(*(const char *const *)a, *(const char *const *)b); }
/* Shipped rows, read once per catalog; guarded by g_editor_rows_lock. */
static SRWLOCK g_editor_rows_lock = SRWLOCK_INIT;
static sh_editor_rows g_editor_rows;
static const void *g_editor_rows_key;
static int g_editor_rows_state;
/* SnapMap's shipped editor entries in catalog (under test, the fixture's
 * names) read into rows: 1, 0 when there is no catalog, -1 on failure. */
static int pr_read_shipped_rows(const sh_resource_catalog *catalog, const pr_original_source *originals,
    sh_editor_rows *rows)
{
    const char **names = NULL; char **owned = NULL; size_t name_count = 0;
    int state = -1;
#ifdef SH_PACKAGE_RUNTIME_TESTING
    /* A fixture that lists no shipped entries ships none; the unavailable
     * switch stands in for a compile without the game's catalog. */
    (void)catalog;
    if (g_test_editor_rows_unavailable) return 0;
    names = (const char **)g_test_editor_rows; name_count = g_test_editor_rows ? g_test_editor_row_count : 0;
#else
    if (!catalog) return 0;
    size_t total = sh_resource_catalog_count(catalog);
    if (!(owned = calloc(total ? total : 1, sizeof(*owned)))) return -1;
    for (size_t i = 0; i < total; i++) {
        const sh_resource_catalog_entry *entry = sh_resource_catalog_at(catalog, i);
        size_t length = entry && entry->path ? strlen(entry->path) : 0;
        if (length <= sizeof(PR_EDITOR_PREFIX) + 4 || strncmp(entry->path, PR_EDITOR_PREFIX, sizeof(PR_EDITOR_PREFIX) - 1) ||
            strcmp(entry->path + length - 5, ".decl")) continue;
        size_t n = length - (sizeof(PR_EDITOR_PREFIX) - 1) - 5;
        if (!(owned[name_count] = malloc(n + 1))) goto done;
        memcpy(owned[name_count], entry->path + sizeof(PR_EDITOR_PREFIX) - 1, n); owned[name_count++][n] = 0;
    }
    if (name_count) qsort(owned, name_count, sizeof(*owned), pr_name_order);
    names = (const char **)owned;
#endif
    for (size_t i = 0; i < name_count; i++) {
        if (i && !strcmp(names[i], names[i - 1])) continue;
        size_t length = strlen(names[i]) + sizeof(PR_EDITOR_PREFIX) + 5;
        char *path = malloc(length);
        unsigned char *body = NULL; size_t size = 0;
        if (!path) goto done;
        snprintf(path, length, PR_EDITOR_PREFIX "%s.decl", names[i]);
        int scope = pr_original((void *)originals, path, &body, &size);
        free(path);
        /* Only SnapMap's own archive supplies the shipped editor roster. */
        if (scope < 0) rows->unreadable++;
        int added = scope == 1 ? sh_editor_rows_add_text(rows, names[i], (const char *)body, size) : 0;
        free(body);
        if (added < 0) goto done;
    }
    if (sh_editor_rows_finish(rows, NULL)) state = 1;
done:
    for (size_t i = 0; owned && i < name_count; i++) free(owned[i]);
    free(owned);
    if (state != 1) sh_editor_rows_free(rows);
    return state;
}
/* The shipped rows for placement, read once per catalog (g_editor_rows_lock). */
static int pr_shipped_rows(const sh_resource_catalog *catalog, const pr_original_source *originals)
{
    const void *key = catalog;
#ifdef SH_PACKAGE_RUNTIME_TESTING
    key = g_test_editor_rows;
#endif
    if (g_editor_rows_state && g_editor_rows_key == key) return g_editor_rows_state;
    sh_editor_rows_free(&g_editor_rows); g_editor_rows_key = key; g_editor_rows_state = -1;
    if (!key) return -1;
    return g_editor_rows_state = pr_read_shipped_rows(catalog, originals, &g_editor_rows) == 1 ? 1 : -1;
}
static int pr_stock_rows(const pr_inline_view *view)
{ return pr_shipped_rows(view->provider->catalog, &view->originals); }

/* ---- Toybox roster for label qualification (strids.h, sh_strids_palette) ---
 * Every editor entry that is a toybox tile, as the current provider serves it:
 * every served editor declaration (package, legacy public and product rows,
 * and packages' own versions of shipped entries), then every shipped entry the
 * provider does not serve. Each resolves its inherited fields through the
 * served entries first and the shipped ones after, so an untouched shipped
 * child of a changed parent shows the parent's served label. Rows read as
 * served, so an aliased row names the label alias its presented text selects
 * (with a map overlay or an installed increment, the one of the resource it
 * serves); label presentation is all this reads, and conversion keeps reading
 * authored bodies. The entries are copies owned here until the next call; the
 * string refresh reads them while it runs (dllmain.c binds this to strids). */
typedef struct pr_roster { sh_strids_palette_entry *entries; char **strings; size_t count, string_count; } pr_roster;
static pr_roster g_roster;
static void pr_roster_free(void)
{
    for (size_t i = 0; i < g_roster.string_count; i++) free(g_roster.strings[i]);
    free(g_roster.strings); free(g_roster.entries); memset(&g_roster, 0, sizeof(g_roster));
}
static const char *pr_roster_copy(const char *value, size_t capacity)
{
    char *copy = _strdup(value);
    if (!copy || g_roster.string_count >= capacity) { free(copy); return NULL; }
    g_roster.strings[g_roster.string_count++] = copy;
    return copy;
}
/* The roster of one compilation (g_roster); the caller keeps it alive. */
static int pr_palette_of(const sh_package_compilation *compiled, const sh_resource_catalog *catalog,
    const pr_original_source *originals, const sh_strids_palette_entry **entries, size_t *count)
{
    sh_editor_rows served = {0};
    int state = 0;
    *entries = NULL; *count = 0;
    pr_roster_free();
    AcquireSRWLockExclusive(&g_editor_rows_lock);
    if (pr_shipped_rows(catalog, originals) != 1) goto done; /* Not yet readable: base labels. */
    state = -1;
    for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = compiled->resources + i;
        size_t length = strlen(resource->engine_path), size = 0;
        char error[256];
        if (length <= sizeof(PR_EDITOR_PREFIX) + 4 || !resource->type ||
            strncmp(resource->engine_path, PR_EDITOR_PREFIX, sizeof(PR_EDITOR_PREFIX) - 1) ||
            strcmp(resource->engine_path + length - 5, ".decl")) continue;
        size_t n = length - (sizeof(PR_EDITOR_PREFIX) - 1) - 5;
        char *name = malloc(n + 1);
        if (!name) goto done;
        memcpy(name, resource->engine_path + sizeof(PR_EDITOR_PREFIX) - 1, n); name[n] = 0;
        unsigned char *body = sh_package_compilation_read_served(compiled, resource, SIZE_MAX / 2, &size, error, sizeof(error));
        int added = body ? sh_editor_rows_add_text(&served, name, (const char *)body, size) : -1;
        free(name); free(body);
        if (added < 0) goto done;
    }
    if (!sh_editor_rows_finish(&served, &g_editor_rows)) goto done;
    size_t total = served.count + g_editor_rows.count, strings = total * 4;
    if (!(g_roster.entries = calloc(total ? total : 1, sizeof(*g_roster.entries))) ||
        !(g_roster.strings = calloc(strings ? strings : 1, sizeof(*g_roster.strings)))) goto done;
    for (size_t i = 0; i < total; i++) {
        const sh_editor_row *row = i < served.count ? served.items + i : g_editor_rows.items + (i - served.count);
        if (i >= served.count && sh_editor_rows_find(&served, row->name)) continue; /* Served above. */
        const char *entity = sh_editor_rows_effective(&served, &g_editor_rows, row, SH_EDITOR_ROW_ENTITY_DEF);
        const char *label = sh_editor_rows_effective(&served, &g_editor_rows, row, SH_EDITOR_ROW_LABEL);
        const char *category = sh_editor_rows_effective(&served, &g_editor_rows, row, SH_EDITOR_ROW_CATEGORY);
        const char *meta = sh_editor_rows_effective(&served, &g_editor_rows, row, SH_EDITOR_ROW_META);
        if (!entity || !*entity || !label || !category || !meta) continue;
        sh_strids_palette_entry *entry = g_roster.entries + g_roster.count;
        if (!(entry->row = pr_roster_copy(row->name, strings)) || !(entry->label = pr_roster_copy(label, strings)) ||
            !(entry->category = pr_roster_copy(category, strings)) || !(entry->meta = pr_roster_copy(meta, strings))) goto done;
        g_roster.count++;
    }
    state = 1;
done:
    ReleaseSRWLockExclusive(&g_editor_rows_lock);
    sh_editor_rows_free(&served);
    if (state != 1) pr_roster_free();
    else { *entries = g_roster.entries; *count = g_roster.count; }
    return state;
}
int sh_package_runtime_palette(void *context, const sh_strids_palette_entry **entries, size_t *count)
{
    (void)context;
    AcquireSRWLockShared(&g_lock);
    pr_original_source originals = {g_catalog ? sh_resource_catalog_read_path : NULL, (void *)g_catalog,
        g_current_provider ? g_current_provider->native_assets : NULL};
#ifdef SH_PACKAGE_RUNTIME_TESTING
    sh_package_runtime_test_source_inputs(NULL, NULL, &originals.game, &originals.context);
#endif
    int state = pr_palette_of(g_compiled, g_catalog, &originals, entries, count);
    ReleaseSRWLockShared(&g_lock);
    return state;
}
#ifdef SH_PACKAGE_RUNTIME_TESTING
int sh_package_runtime_test_palette_of(const sh_package_compilation *compiled,
    const struct sh_strids_palette_entry **entries, size_t *count)
{
    pr_original_source originals = {NULL, NULL, NULL};
    sh_package_runtime_test_source_inputs(NULL, NULL, &originals.game, &originals.context);
    return pr_palette_of(compiled, NULL, &originals, entries, count);
}
#endif
static int pr_compile_rows_read(void *context, const struct sh_editor_rows **rows)
{
    pr_compile_rows *compile = context;
    if (!compile->state) {
        int read = pr_read_shipped_rows(compile->catalog, compile->originals, &compile->rows);
        /* A descendant closure needs every shipped entry: one the archive holds
         * but that cannot be read or parsed fails the roster (an absent or
         * campaign-only name is simply not a SnapMap entry). */
        if (read == 1 && compile->rows.unreadable) { sh_editor_rows_free(&compile->rows); read = -1; }
        compile->state = read == 1 ? 1 : read == 0 ? 2 : -1;
    }
    *rows = compile->state == 1 ? &compile->rows : NULL;
    return compile->state == 1 ? 1 : compile->state == 2 ? 0 : -1;
}
/* Editor rows this map's packages serve, read once per conversion. */
static int pr_package_rows(pr_inline_view *view)
{
    const sh_package_compilation *compiled = view->provider->compiled;
    char error[256];
    const char **context_copy = NULL, **shipped_name = NULL; /* private name -> shipped name */
    size_t copies = 0;
    int state = -1;
    if (view->package_rows_state) return view->package_rows_state;
    view->package_rows_state = -1;
    context_copy = calloc(compiled->resource_count ? compiled->resource_count : 1, sizeof(*context_copy));
    shipped_name = calloc(compiled->resource_count ? compiled->resource_count : 1, sizeof(*shipped_name));
    if (!context_copy || !shipped_name) goto done;
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = compiled->resources + i;
        size_t length = strlen(resource->engine_path), size = 0;
        if (resource->native_original || resource->restored_original ||
            strncmp(resource->engine_path, PR_EDITOR_PREFIX, sizeof(PR_EDITOR_PREFIX) - 1) ||
            length <= sizeof(PR_EDITOR_PREFIX) + 4 || strcmp(resource->engine_path + length - 5, ".decl")) continue;
        /* The installed library serves the stock editor entries its packages
         * inherit under their own names, composed from the originals alone:
         * no package owns a change to them. A plain package entry names such an
         * entry, so in any context it presents exactly as that stock entry does;
         * these copies stand for the shipped rows. A stock entry a package does
         * change (an owner) stays a package row, refusing its plain copies. */
        if (resource->baseline_known && !sh_package_owners_count(&resource->owners) &&
            (!resource->original_path || !_stricmp(resource->original_path, resource->engine_path))) continue;
        /* The same unowned stock entry, linked privately for a package's own
         * context (a shared editor declaration it references differs there):
         * the converted entity is placed by the shipped entry itself, so the
         * copy stands for it too. */
        size_t original_length = resource->original_path ? strlen(resource->original_path) : 0;
        if (resource->baseline_known && !sh_package_owners_count(&resource->owners) &&
            original_length > sizeof(PR_EDITOR_PREFIX) + 4 &&
            !_strnicmp(resource->original_path, PR_EDITOR_PREFIX, sizeof(PR_EDITOR_PREFIX) - 1) &&
            !_stricmp(resource->original_path + original_length - 5, ".decl")) {
            context_copy[copies] = resource->engine_path + sizeof(PR_EDITOR_PREFIX) - 1;
            shipped_name[copies++] = resource->original_path + sizeof(PR_EDITOR_PREFIX) - 1;
            continue;
        }
        unsigned char *body = sh_package_compilation_read(compiled, resource, SIZE_MAX / 2, &size, error, sizeof(error));
        if (!body) goto done;
        size_t n = length - (sizeof(PR_EDITOR_PREFIX) - 1) - 5;
        char *name = malloc(n + 1);
        if (!name) { free(body); goto done; }
        memcpy(name, resource->engine_path + sizeof(PR_EDITOR_PREFIX) - 1, n); name[n] = 0;
        int added = sh_editor_rows_add_text(&view->package_rows, name, (const char *)body, size);
        free(name); free(body);
        if (added < 0) goto done;
    }
    /* Package entries inheriting such a copy inherit the shipped entry. */
    for (size_t i = 0; i < view->package_rows.count; i++) {
        sh_editor_row *row = view->package_rows.items + i;
        for (size_t c = 0; row->parent && c < copies; c++) {
            size_t n = strlen(context_copy[c]) - 5, m = strlen(shipped_name[c]) - 5;
            if (strlen(row->parent) != n || _strnicmp(row->parent, context_copy[c], n)) continue;
            char *parent = malloc(m + 1);
            if (!parent) goto done;
            memcpy(parent, shipped_name[c], m); parent[m] = 0;
            free(row->parent); row->parent = parent;
            break;
        }
    }
    state = view->package_rows_state = sh_editor_rows_finish(&view->package_rows, &g_editor_rows) ? 1 : -1;
done:
    free((void *)context_copy); free((void *)shipped_name);
    return state;
}
static const char *pr_inline_placement(void *context, const char *from, const char *base, sh_decl_carrier_limit *limit)
{
    pr_inline_view *view = context;
    const char *reason = view->placement;
    char authored[512] = "";
    *limit = SH_DECL_CARRIER_GAP;
    AcquireSRWLockExclusive(&g_editor_rows_lock);
    if (pr_stock_rows(view) != 1) {
        snprintf(view->placement, sizeof(view->placement),
            "SnapMap's shipped editor entries could not be read to prove '%s' is placeable", base);
        goto done;
    }
    /* The authored spelling of a served private definition. */
    size_t length = strlen(from) + sizeof("generated/decls/entitydef/.decl");
    char *path = malloc(length);
    if (!path) { snprintf(view->placement, sizeof(view->placement), "placement could not be checked"); goto done; }
    snprintf(path, length, "generated/decls/entitydef/%s.decl", from);
    const sh_compiled_resource *served = sh_package_compilation_find(view->provider->compiled, path);
    free(path);
    const char *original = served ? served->original_path : NULL;
    size_t n = original ? strlen(original) : 0;
    if (n > 31 && !strncmp(original, "generated/decls/entitydef/", 26) && !strcmp(original + n - 5, ".decl") &&
        n - 31 < sizeof(authored)) { memcpy(authored, original + 26, n - 31); authored[n - 31] = 0; }
    if (pr_package_rows(view) != 1) {
        snprintf(view->placement, sizeof(view->placement), "its package's editor entries could not be read");
        goto done;
    }
    reason = sh_editor_rows_placement(&g_editor_rows, &view->package_rows, from, authored[0] ? authored : NULL,
        base, limit, view->placement, sizeof(view->placement));
done:
    ReleaseSRWLockExclusive(&g_editor_rows_lock);
    return reason;
}
#ifdef SH_PACKAGE_RUNTIME_TESTING
int sh_package_runtime_test_placement(const sh_package_compilation *compiled, const char *from, const char *base,
    int *engine, char *reason, size_t capacity)
{
    pr_provider provider; pr_inline_view view;
    sh_decl_carrier_limit limit = SH_DECL_CARRIER_GAP;
    memset(&provider, 0, sizeof(provider)); memset(&view, 0, sizeof(view));
    provider.compiled = (sh_package_compilation *)compiled;
    view.provider = &provider;
    sh_package_runtime_test_source_inputs(NULL, NULL, &view.originals.game, &view.originals.context);
    const char *why = pr_inline_placement(&view, from, base, &limit);
    snprintf(reason, capacity, "%s", why ? why : "");
    *engine = limit == SH_DECL_CARRIER_ENGINE;
    sh_editor_rows_free(&view.package_rows);
    return why ? (view.package_rows_state < 0 ? -1 : 0) : 1;
}
#endif

/* Whether a delivery the derived map would no longer select still carries
 * behavior the map needs. Unused bundle members are no requirement: package
 * definitions, stock-named edits included, compile under private identities
 * that only bound typed uses reach, and the recomputed selection keeps every
 * such use. Spawn registrations are private rows that only the package's own
 * bound requests select. What remains is shared state: a string the derived
 * map still names, a stock definition changed under its stock name (historical
 * composed contexts), cvar requirements, historical string keys and HUD
 * settings for shared weapons. The map needing one of them is a native limit,
 * since map data carries none of them. A shared effect whose consumers are not
 * modelled is unproven: a product gap. Returns 1 to keep (limit/reason set). */
static int pr_inline_published(const sh_compiled_resource *resource)
{
    const char *original = resource->original_path, *slash, *dot;
    char type[64], name[512];
    if (!original || !_stricmp(resource->engine_path, original)) return 1;
    if (strncmp(original, "generated/decls/", 16)) return 0;
    slash = strchr(original + 16, '/'); dot = strrchr(original, '.');
    if (!slash || !dot || dot <= slash || (size_t)(slash - original - 16) >= sizeof(type) ||
        (size_t)(dot - slash - 1) >= sizeof(name)) return 1;
    memcpy(type, original + 16, (size_t)(slash - original - 16)); type[slash - original - 16] = 0;
    memcpy(name, slash + 1, (size_t)(dot - slash - 1)); name[dot - slash - 1] = 0;
    return sh_package_resource_native_matches(resource, type, name);
}
/* A weapon identity every map shares: unchanged stock, or changed under its
 * stock name. Private linked weapons are reached only through package uses. */
static int pr_inline_shared_weapon(const pr_provider *provider, const char *name)
{
    size_t length = strlen(name) + sizeof("generated/decls/weapon/.decl");
    char *path = malloc(length);
    if (!path) return -1;
    snprintf(path, length, "generated/decls/weapon/%s.decl", name);
    const sh_compiled_resource *resource = sh_package_compilation_find(provider->compiled, path);
    free(path);
    if (!resource) return strncmp(name, "smp/", 4) != 0;
    return resource->baseline_known && pr_inline_published(resource);
}
static int pr_inline_mentions(const char *json, size_t length, const char *key)
{
    size_t n = strlen(key);
    for (size_t i = 1; n && json && i + n < length; i++)
        if (json[i - 1] == '"' && json[i + n] == '"' && !_strnicmp(json + i, key, n)) return 1;
    return 0;
}
/* A string key of the package's policy that the derived map still names. */
static int pr_inline_named_string(const sh_package_policy *policy, const char *json, size_t length,
    char *key, size_t capacity)
{
    int found = 0;
    for (size_t i = 0; !found && i < policy->strings.count; i++) {
        const char *raw = policy->strings.members[i].value_json;
        sh_json_object pairs = {0};
        if (!sh_json_parse_object(raw, strlen(raw), 2, &pairs)) return -1;
        for (size_t j = 0; !found && j < pairs.count; j++) {
            int n = snprintf(key, capacity, "#str_%s", pairs.members[j].key);
            found = n > 0 && (size_t)n < capacity && pr_inline_mentions(json, length, key);
        }
        sh_json_object_free(&pairs);
    }
    for (size_t i = 0; !found && i < policy->localized_count; i++)
        if (pr_inline_mentions(json, length, policy->localized[i].key)) {
            snprintf(key, capacity, "%s", policy->localized[i].key); found = 1;
        }
    return found;
}
static int pr_inline_requirement(const pr_provider *provider, const pr_original_source *originals, size_t package,
    const sh_package_references *closure, const char *derived, size_t derived_length, pr_inline_kept *kept)
{
    sh_package_owners owners = {0};
    sh_package_policy policy = {0};
    char error[256], key[256];
    const char *changed = NULL, *weapon = NULL;
    int inspected = sh_package_owners_add(&owners, package) &&
        sh_package_compilation_policy(provider->compiled, &owners, &policy, error, sizeof(error));
    sh_package_owners_free(&owners);
    kept->limit = SH_DECL_CARRIER_GAP;
    if (!inspected) {
        snprintf(kept->reason, sizeof(kept->reason), "its package's shared settings could not be inspected");
        return 1;
    }
    int named = pr_inline_named_string(&policy, derived, derived_length, key, sizeof(key));
    if (named) {
        kept->limit = named < 0 ? SH_DECL_CARRIER_GAP : SH_DECL_CARRIER_ENGINE;
        if (named < 0) snprintf(kept->reason, sizeof(kept->reason), "its package's strings could not be inspected");
        else snprintf(kept->reason, sizeof(kept->reason),
            "the map still names %s, a string its package supplies; map data cannot define strings", key);
        sh_package_policy_free(&policy); return 1;
    }
    for (size_t i = 0; i < provider->compiled->resource_count; i++) {
        const sh_compiled_resource *resource = provider->compiled->resources + i;
        if (!resource->baseline_known || resource->native_original || resource->restored_original ||
            !sh_package_owners_contains(&resource->gameplay_owners, package) || !pr_inline_published(resource)) continue;
        int same = pr_stock_copy(originals, provider->compiled, resource);
        if (same < 0) { sh_package_policy_free(&policy); return -1; }
        if (same) continue;
        const char *shown = resource->original_path ? resource->original_path : resource->engine_path;
        for (size_t r = 0; closure && r < closure->count; r++)
            if (closure->items[r].name && (!_stricmp(closure->items[r].name, resource->engine_path) ||
                (resource->original_path && !_stricmp(closure->items[r].name, resource->original_path)))) {
                kept->limit = SH_DECL_CARRIER_ENGINE;
                snprintf(kept->reason, sizeof(kept->reason), "the map still reaches %s, which its package changes "
                    "under the stock name; map data cannot change stock definitions", shown);
                sh_package_policy_free(&policy); return 1;
            }
        if (!changed) changed = shown;
    }
    for (size_t i = 0; !weapon && i < policy.hud.count + policy.hud_binding_count; i++) {
        const char *name = i < policy.hud.count ? policy.hud.members[i].key : policy.hud_bindings[i - policy.hud.count].name;
        int shared = pr_inline_shared_weapon(provider, name);
        if (shared < 0) { sh_package_policy_free(&policy); return -1; }
        if (shared) weapon = name;
    }
    int keep = 1;
    if (policy.requirements.count)
        snprintf(kept->reason, sizeof(kept->reason),
            "its package lifts SnapMap's resource blacklist, and whether the native map needs that is not modelled");
    else if (policy.strings.count && sh_package_owners_contains(&provider->compiled->policy.legacy_string_owners, package))
        snprintf(kept->reason, sizeof(kept->reason),
            "its package's historical strings use shared keys whose other consumers are not modelled");
    else if (weapon)
        snprintf(kept->reason, sizeof(kept->reason),
            "its package sets HUD presentation for the shared weapon %s, whose use is not modelled", weapon);
    else if (changed)
        snprintf(kept->reason, sizeof(kept->reason), closure && !closure->incomplete ?
            "its package changes %s under the stock name, and consumers outside this map's references are not modelled" :
            "its package changes %s under the stock name, and the map's dependency coverage is incomplete", changed);
    else keep = 0;
    sh_package_policy_free(&policy);
    return keep;
}

static int pr_inline_add_kept(pr_inline_view *view, const unsigned char tree[32], const pr_inline_kept *kept)
{
    for (size_t i = 0; i < view->kept_count; i++) if (!memcmp(view->kept[i].tree, tree, 32)) return 1;
    pr_inline_kept *grown = realloc(view->kept, (view->kept_count + 1) * sizeof(*grown));
    if (!grown) return 0;
    view->kept = grown; view->kept[view->kept_count] = *kept;
    memcpy(view->kept[view->kept_count++].tree, tree, 32);
    return 1;
}

static char *pr_inline_report(const sh_native_inline_report *report)
{
    size_t length = 1;
    for (size_t i = 0; i < report->count; i++) length += 640;
    char *text = malloc(length), *at = text;
    if (!text) return NULL;
    *at = 0;
    for (size_t i = 0; i < report->count; i++) {
        const sh_native_inline_site *site = report->sites + i;
        int written = site->converted ?
            snprintf(at, length - (size_t)(at - text), "entity %zu: %s -> native %s\n", site->entity, site->from, site->to) :
            snprintf(at, length - (size_t)(at - text), "entity %zu: %s kept (%s): %s\n", site->entity, site->from,
                site->limit == SH_DECL_CARRIER_ENGINE ? "native limit" : "product gap", site->reason);
        if (written > 0) at += written;
    }
    return text;
}

static int pr_native_document(const pr_provider *provider, sh_decl_registry_source registry, uintptr_t reflection,
    sh_decl_native_source metadata, int retain_native, const sh_package_provenance *saved,
    const sh_package_bundle_binding *additional, size_t additional_count, const char *json, size_t length,
    char **derived, size_t *derived_length, sh_package_occurrence_snapshot **ownership,
    sh_mpkg_used **deliveries, size_t *delivery_count, pr_key_context *keys, char **report,
    char *error, size_t capacity)
{
    pr_inline_view view = {0};
    sh_package_occurrence_snapshot *live = NULL, *next = NULL;
    sh_mpkg_used *live_used = NULL, *next_used = NULL;
    size_t live_count = 0, next_count = 0, live_length = 0, next_length = 0;
    char *live_json = NULL, *next_json = NULL, *output = NULL;
    sh_native_inline_report sites = {0};
    sh_package_references closure = {0};
    int result = -1;
    if (!provider || !provider->compiled) goto done;
    live_json = pr_capture_document(provider, registry, reflection, retain_native, saved, additional, additional_count,
        json, length, &live_length, &live, &live_used, &live_count, NULL, keys, error, capacity);
    if (!live_json) goto done;
    view.provider = provider; view.uses = sh_package_occurrence_provenance(live);
    view.originals = (pr_original_source){provider->catalog ? sh_resource_catalog_read_path : NULL,
        provider->catalog, provider->native_assets};
#ifdef SH_PACKAGE_RUNTIME_TESTING
    sh_package_runtime_test_source_inputs(NULL, NULL, &view.originals.game, &view.originals.context);
#endif
    metadata.reflection = reflection;
    view.schema = sh_decl_native_schema_open(metadata, (sh_decl_dependency_observer){0}, error, capacity);
    if (!view.schema) goto done;
    sh_native_inline_source source = {&view, pr_inline_read, pr_inline_derives,
        sh_decl_native_schema_view(view.schema), pr_inline_looked_up, pr_inline_keep, PR_ENTITY_STATE_VERSION,
        pr_inline_placement};
    for (int pass = 0; pass < 8; pass++) {
        size_t converted = 0, output_length = 0;
        sh_native_inline_report_free(&sites);
        free(output); output = sh_native_inline_document(live_json, live_length, &source, &output_length, &sites, error, capacity);
        if (!output) goto done;
        for (size_t i = 0; i < sites.count; i++) converted += sites.sites[i].converted != 0;
        if (!converted) { result = 0; goto done; }
        sh_package_occurrence_release(next); next = NULL;
        sh_mpkg_used_free(next_used); next_used = NULL; free(next_json);
        sh_package_references_free(&closure);
        next_json = pr_capture_document(provider, registry, reflection, retain_native, saved, additional, additional_count,
            output, output_length, &next_length, &next, &next_used, &next_count, &closure, keys, error, capacity);
        if (!next_json) goto done;
        /* Recompute ownership: a delivery the derived map drops must not take
         * behavior the map still needs with it; keep its uses and try again. */
        size_t kept_before = view.kept_count;
        for (size_t i = 0; i < live_count; i++) {
            int present = 0;
            for (size_t j = 0; j < next_count && !present; j++) present = !strcmp(live_used[i].id, next_used[j].id);
            if (present) continue;
            const unsigned char *tree = sh_package_bundle_fingerprint(live_used[i].bundle);
            if (!tree) goto done;
            size_t package = SIZE_MAX;
            for (size_t p = 0; p < provider->bundle_count; p++) {
                const unsigned char *candidate = sh_package_bundle_fingerprint(provider->bundles[p]);
                if (candidate && !memcmp(candidate, tree, 32)) { package = p; break; }
            }
            pr_inline_kept kept = {{0}, SH_DECL_CARRIER_GAP, "a retained copy's delivery cannot be inspected here"};
            int keep = package == SIZE_MAX ? 1 :
                pr_inline_requirement(provider, &view.originals, package, &closure, next_json, next_length, &kept);
            if (keep < 0 || (keep && !pr_inline_add_kept(&view, tree, &kept))) goto done;
        }
        if (view.kept_count == kept_before) {
            *derived = next_json; next_json = NULL; *derived_length = next_length;
            sh_package_occurrence_release(*ownership); *ownership = next; next = NULL;
            sh_mpkg_used_free(*deliveries); *deliveries = next_used; next_used = NULL; *delivery_count = next_count;
            result = 1; goto done;
        }
    }
    if (error && capacity) snprintf(error, capacity, "native representation did not settle");
done:
    if (report && sites.count) { free(*report); *report = pr_inline_report(&sites); }
    sh_native_inline_report_free(&sites);
    if (view.schema) sh_decl_native_schema_close(view.schema);
    sh_editor_rows_free(&view.package_rows);
    free(view.kept); free(output); free(live_json); free(next_json); sh_package_references_free(&closure);
    sh_package_occurrence_release(live); sh_package_occurrence_release(next);
    sh_mpkg_used_free(live_used); sh_mpkg_used_free(next_used);
    return result;
}

int sh_package_runtime_native_document(const sh_package_map_plan *plan,
    const sh_package_provenance *saved, const sh_package_bundle_binding *additional,
    size_t additional_count, const char *json, size_t length,
    char **derived, size_t *derived_length, sh_package_occurrence_snapshot **ownership,
    sh_mpkg_used **deliveries, size_t *delivery_count, char **report,
    char *error, size_t capacity)
{
    int result = -1;
    char *pending = NULL; size_t pending_length = 0, pending_count = 0;
    sh_package_occurrence_snapshot *pending_ownership = NULL;
    sh_mpkg_used *pending_deliveries = NULL;
    if (error && capacity) *error = 0;
    if (!derived || !derived_length || !ownership || !deliveries || !delivery_count || !json) return -1;
    if (!g_activation_callback) AcquireSRWLockExclusive(&g_refresh_lock);
    AcquireSRWLockShared(&g_lock);
    pr_key_context keys;
    pr_key_context_open(&keys, plan ? plan->provider : g_map_provider ? g_map_provider : g_library_provider);
#ifndef SH_PACKAGE_RUNTIME_TESTING
    sh_decl_registry_source registry = {0};
    sh_decl_native_source metadata = {NULL, pr_native_read, 0, g_native_scripts};
    if (sh_decl_server_registry_source(&registry) && sh_decl_native_source_ready(&metadata,
        g_native_accessor, g_native_types, g_native_game_system) == 1)
        result = pr_native_document(plan ? plan->provider : g_current_provider, registry, metadata.reflection, metadata,
            !plan, saved, additional, additional_count, json, length, &pending, &pending_length,
            &pending_ownership, &pending_deliveries, &pending_count, &keys, report, error, capacity);
#else
    sh_decl_registry_source registry = {0}; uintptr_t reflection = 0;
    sh_decl_native_source metadata = {NULL, pr_native_read, 0, NULL};
    if (sh_package_runtime_test_source_inputs(&registry, &reflection, NULL, NULL) && (metadata.scripts = registry.scripts, 1))
        result = pr_native_document(plan ? plan->provider : g_current_provider, registry, reflection, metadata,
            !plan, saved, additional, additional_count, json, length, &pending, &pending_length,
            &pending_ownership, &pending_deliveries, &pending_count, &keys, report, error, capacity);
#endif
    ReleaseSRWLockShared(&g_lock);
    if (!pr_key_context_close(&keys, result == 1)) {
        if (result == 1 && error && capacity) snprintf(error, capacity, "map registration dictionary could not be published");
        if (result == 1) result = -1;
    }
    if (result == 1) {
        free(*derived); *derived = pending; pending = NULL; *derived_length = pending_length;
        sh_package_occurrence_release(*ownership); *ownership = pending_ownership; pending_ownership = NULL;
        sh_mpkg_used_free(*deliveries); *deliveries = pending_deliveries; pending_deliveries = NULL;
        *delivery_count = pending_count;
    }
    free(pending); sh_package_occurrence_release(pending_ownership); sh_mpkg_used_free(pending_deliveries);
    if (!g_activation_callback) ReleaseSRWLockExclusive(&g_refresh_lock);
    return result;
}

#ifdef SH_PACKAGE_RUNTIME_TESTING
int sh_package_runtime_test_unmodified_type(uint32_t type)
{
#ifdef PR_NATIVE_SPAWN
    return sh_spawn_keys_test_unmodified(type);
#else
    int found = 0;
    AcquireSRWLockShared(&g_lock);
    for (size_t i = 0; g_current_provider && i < g_current_provider->unmodified_count; i++)
        if (g_current_provider->unmodified_types[i] == type) { found = 1; break; }
    ReleaseSRWLockShared(&g_lock); return found;
#endif
}
void sh_package_runtime_test_plan_key_table(sh_package_map_plan *plan, const sh_package_key_groups *table)
{
    if (!plan || !plan->provider) abort();
    free(plan->provider->key_groups); plan->provider->key_groups = NULL;
    if (table) {
        plan->provider->key_groups = malloc(sizeof(*table));
        if (!plan->provider->key_groups) abort();
        *plan->provider->key_groups = *table;
    }
}
void sh_package_runtime_test_serving_key_table(sh_package_key_groups *table)
{
    AcquireSRWLockShared(&g_lock);
    memset(table, 0, sizeof(*table));
    if (g_current_provider && g_current_provider->key_groups) *table = *g_current_provider->key_groups;
    ReleaseSRWLockShared(&g_lock);
}
void sh_package_runtime_test_key_tables(const sh_package_key_groups *library, const sh_package_key_groups *map)
{
    AcquireSRWLockExclusive(&g_lock);
    pr_provider *owners[2] = {g_library_provider, g_map_provider};
    const sh_package_key_groups *tables[2] = {library, map};
    for (int i = 0; i < 2; i++) if (owners[i]) {
        free(owners[i]->key_groups); owners[i]->key_groups = NULL;
        if (tables[i] && (owners[i]->key_groups = malloc(sizeof(*tables[i])))) *owners[i]->key_groups = *tables[i];
    }
    if (g_current_provider && g_current_provider != g_library_provider && g_current_provider != g_map_provider) {
        char error[256] = "";
        free(g_current_provider->key_groups); g_current_provider->key_groups = NULL;
        if (!pr_key_groups_add(g_current_provider, g_library_provider, error, sizeof(error)) ||
            !pr_key_groups_add(g_current_provider, g_map_provider, error, sizeof(error))) abort();
    }
    if (g_current_provider) {
        char error[256];
        if (!pr_prepare_spawn_program(g_current_provider, error, sizeof(error))) abort();
    }
    pr_publish_key_groups();
    ReleaseSRWLockExclusive(&g_lock);
}
int sh_package_runtime_test_key_groups_current(void)
{
    AcquireSRWLockShared(&g_lock);
    int current = g_test_key_groups[0] == g_current_provider &&
        g_test_key_groups[1] == (g_current_provider ? g_current_provider->key_groups : NULL);
#ifdef PR_NATIVE_SPAWN
    sh_package_key_groups empty = {0};
    const sh_package_key_groups *expected = g_current_provider && g_current_provider->key_groups ?
        g_current_provider->key_groups : &empty;
    const sh_package_key_groups *published = sh_spawn_keys_test_hold();
    current = current && !memcmp(expected, published, sizeof(*expected));
    sh_spawn_keys_test_release();
#endif
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
    sh_package_link_history_close(g_link_history); g_link_history = NULL;
    sh_package_source_memo_close(g_source_memo); g_source_memo = NULL;
    ReleaseSRWLockExclusive(&g_refresh_lock);
}
#endif
