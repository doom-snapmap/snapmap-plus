/* Pack package folders and detect their published decl references in map
 * JSON.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map_embed.h"
#include "packages.h"
#include "overrides.h"
#include "backend_log.h"

#include "package_archive.h"

unsigned char *sh_mpkg_pack_used(const sh_mpkg_used *used, size_t *out_len, char *error, size_t capacity)
{
    unsigned char *bytes, *payload = NULL;
    size_t length = 0;
    char delivery[SH_PACKAGE_BUNDLE_KEY_CAP];
    if (out_len) *out_len = 0;
    if (error && capacity) error[0] = 0;
    if (!used || !used->bundle || !out_len) {
        if (error && capacity) snprintf(error, capacity, "the selected complete package snapshot is unavailable");
        return NULL;
    }
    sh_package_bundle_delivery_key(used->bundle, delivery);
    if (strcmp(delivery, used->id)) {
        if (error && capacity) snprintf(error, capacity, "selected delivery does not match its retained original tree");
        return NULL;
    }
    bytes = sh_package_bundle_read(used->bundle, &length, error, capacity);
    if (!bytes) return NULL;
    payload = HeapAlloc(GetProcessHeap(), 0, length);
    if (payload) { memcpy(payload, bytes, length); *out_len = length; }
    else if (error && capacity) snprintf(error, capacity, "cannot allocate retained complete package delivery");
    free(bytes); return payload;
}


unsigned char *sh_mpkg_pack_dir(const char *root, size_t *out_len, char *err, size_t err_cap)
{
    sh_package_sources *sources = sh_package_sources_scan_directory(root, err, err_cap);
    unsigned char *packed, *out = NULL;
    size_t length = 0;
    if (out_len) *out_len = 0;
    if (!sources) return NULL;
    packed = sh_package_archive_pack(sources, 0, &length, err, err_cap);
    sh_package_sources_free(sources);
    if (!packed) return NULL;
    out = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, length);
    if (out) { memcpy(out, packed, length); if (out_len) *out_len = length; }
    else if (err && err_cap) snprintf(err, err_cap, "out of memory packing complete package");
    free(packed); return out;
}

#include "package_runtime.h"
#include "package_usage.h"
#include "decl_server.h"
#include "typeinfo.h"
#include "resource_graph_prepared.h"
#include "package_link_inputs.h"

static int embed_prepare_reference(void *context, const char *type, const char *name)
{
    if (!_stricmp(type, "entitydef"))
        sh_resource_graph_prepare_entity((sh_resource_graph_preparation *)context, name, NULL);
    return 1;
}
typedef struct embed_preparation {
    sh_resource_graph_preparation *native;
    sh_package_references *references;
    char *error;
    size_t error_capacity;
} embed_preparation;

static int embed_prepare_state(void *context, const char *class_name, const char *inherit,
    const char *edit, size_t length)
{
    embed_preparation *preparation = context;
    sh_decl_dependency_result result = {0};
    if (!sh_resource_graph_prepare_inline(preparation->native, class_name, inherit, edit, length,
            sh_package_references_add, preparation->references, &result))
        preparation->references->incomplete = 1;
    if (result.aborted && preparation->error && preparation->error_capacity)
        snprintf(preparation->error, preparation->error_capacity,
            "Inline dependency traversal failed for class '%s', inherit '%s'.", class_name, inherit);
    return !result.aborted;
}

int sh_mpkg_prepare_map(const char *json, size_t length, sh_package_references *references,
                        char *error, size_t error_capacity)
{
    sh_decl_registry_source source;
    embed_preparation preparation = {0};
    uintptr_t reflection;
    int ok;
    sh_json_error problem;
    if (error && error_capacity) error[0] = 0;
    if (!references || (!json && length)) {
        if (error && error_capacity) snprintf(error, error_capacity, "Map dependency input is unavailable.");
        return 0;
    }
    if (!json) return 1;
    if (!sh_native_json_validate(json, length, 128, NULL, &problem)) {
        if (error && error_capacity) snprintf(error, error_capacity,
            "Map JSON rejected at byte %zu: %s.", problem.offset, problem.reason);
        return 0;
    }
    if (!sh_decl_server_registry_source(&source) ||
        !(reflection = (uintptr_t)sh_typeinfo_get_reflect())) { references->incomplete = 1; return 1; }
    preparation.references = references;
    preparation.error = error; preparation.error_capacity = error_capacity;
    preparation.native = sh_resource_graph_prepare_open(source, reflection);
    if (!preparation.native) { references->incomplete = 1; return 1; }
    ok = sh_package_map_states(json, length, embed_prepare_state, &preparation);
    if (!ok && error && error_capacity && !error[0])
        snprintf(error, error_capacity, "Map entity-state dependency traversal failed.");
    if (ok) {
        ok = sh_package_map_references(json, length, embed_prepare_reference, preparation.native);
        if (!ok && error && error_capacity)
            snprintf(error, error_capacity, "Map resource-reference traversal failed.");
    }
    if (ok) {
        size_t i;
        for (i = 0; i < references->count; i++)
            embed_prepare_reference(preparation.native, references->items[i].type, references->items[i].name);
    }
    sh_resource_graph_prepare_close(preparation.native);
    if (!ok) sh_package_references_free(references);
    return ok;
}

static int embed_reference_order(const void *a, const void *b)
{
    const sh_package_reference *x = a, *y = b;
    int order = strcmp(x->type, y->type); return order ? order : strcmp(x->name, y->name);
}
static int embed_binding_used(void *context, const char *family, const char *name)
{
    const sh_package_references *used = context;
    sh_package_reference key = {(char *)family, (char *)name};
    return used->count && bsearch(&key, used->items, used->count, sizeof(key), embed_reference_order) != NULL;
}
static int embed_bindings(const sh_package_compilation *compiled, const sh_resource_catalog *catalog,
    const sh_package_provenance *index, const char *json, size_t len,
    const sh_package_references *references, sh_package_owners *owners, sh_package_provenance *out)
{
    sh_package_references paths = {0}, used = {0};
    int ok = 0, isolated = 0;
    if (!index || !index->binding_count) return 1;
    if (!sh_package_map_resources(compiled, catalog, json, len, references, &paths)) goto done;
    for (size_t i = 0; i < paths.count; i++) {
        const sh_compiled_resource *resource = sh_package_compilation_find(compiled, paths.items[i].name);
        if (!resource || (!sh_package_owners_count(&resource->gameplay_owners) &&
            (!resource->original_path || !strcmp(resource->original_path, resource->engine_path)))) continue;
        size_t native_count = sh_package_resource_native_count(resource);
        if (native_count) {
            for (size_t n = 0; n < native_count; n++) {
                sh_package_native_identity native = sh_package_resource_native_at(resource, n);
                if (!sh_package_references_add(&used, native.type, native.name)) goto done;
            }
        } else {
            const sh_resource_catalog_entry *const *entries = NULL;
            size_t count = sh_resource_catalog_find_path(catalog, resource->engine_path, &entries);
            for (size_t e = 0; e < count; e++) if (*entries[e]->type && *entries[e]->name &&
                !sh_package_references_add(&used, entries[e]->type, entries[e]->name)) goto done;
            if (!count) {
                char *cooked = NULL;
                const char *family = NULL;
                int status = sh_package_cooked_identity(resource->engine_path, &family, &cooked);
                int added = status != 1 || sh_package_references_add(&used, family, cooked);
                free(cooked); if (status < 0 || !added) goto done;
            }
        }
    }
    if (used.count > 1) qsort(used.items, used.count, sizeof(*used.items), embed_reference_order);
    if (!sh_package_provenance_select(index, embed_binding_used, &used, out)) goto done;
    for (size_t i = 0; i < out->context_count; i++)
        if (out->contexts[i].mode == SH_PACKAGE_CONTEXT_ISOLATED) isolated = 1;
    /* An unchanged old map must never acquire new linker requirements merely
     * from saving. A mixed map records its old composition as one context. */
    if (!isolated) { sh_package_provenance_free(out); ok = 1; goto done; }
    for (size_t i = 0; i < out->context_count; i++) {
        const sh_package_provenance_context *scope = out->contexts + i;
        for (size_t t = 0; t < scope->tree_count; t++) {
            size_t owner = SIZE_MAX;
            for (size_t p = 0; p < compiled->sources->package_count; p++) {
                if (!compiled->sources->fingerprints || memcmp(scope->trees[t], compiled->sources->fingerprints[p], 32)) continue;
                if (sh_package_owners_contains(owners, p)) { owner = p; break; }
                if (owner == SIZE_MAX || (compiled->map_overlay && p >= compiled->map_owner_begin)) owner = p;
            }
            if (owner == SIZE_MAX || !sh_package_owners_add(owners, owner)) goto done;
        }
    }
    ok = 1;
done:
    sh_package_references_free(&paths); sh_package_references_free(&used);
    if (!ok) sh_package_provenance_free(out);
    return ok;
}

size_t sh_mpkg_used_packages_with_bindings(const char *json, size_t len, const char *data_root,
    sh_mpkg_used **out, sh_package_provenance *bindings, char *error, size_t error_capacity)
{
    const sh_package_compilation *compiled;
    sh_package_owners owners = {0};
    int dependencies_complete = 0;
    sh_package_references references = {0};
    size_t count = 0, i, capacity;
    sh_mpkg_used *selected = NULL;
    const char *failure = "Map package inventory output is unavailable.";
    sh_package_provenance_free(bindings);
    if (error && error_capacity) error[0] = 0;
    if (!out) {
        if (error && error_capacity) snprintf(error, error_capacity, "%s", failure);
        return SIZE_MAX;
    }
    sh_mpkg_used_free(*out); *out = NULL;
    (void)data_root;
    /* Source probes may enter the engine. Finish this pass before acquiring
     * the immutable compiler snapshot used for package ownership. */
    if (!sh_mpkg_prepare_map(json, len, &references, error, error_capacity)) return SIZE_MAX;
    compiled = sh_package_runtime_acquire();
    failure = "The compiled package library is unavailable.";
    if (!compiled) goto bad;
    failure = "Map gameplay resource ownership traversal failed.";
    if (!sh_package_map_owners(compiled, sh_package_runtime_catalog(), json, len,
                               &references, &owners, &dependencies_complete)) goto bad;
    failure = "Cannot retain the map's supplying source contexts.";
    if (bindings && !embed_bindings(compiled, sh_package_runtime_catalog(), sh_package_runtime_bindings(),
        json, len, &references, &owners, bindings)) goto bad;
    capacity = sh_package_owners_count(&owners);
    failure = "Cannot allocate the map's supplying package list.";
    if (capacity >= SIZE_MAX / sizeof(*selected) ||
        (capacity && !(selected = (sh_mpkg_used *)calloc(capacity + 1u, sizeof(*selected))))) goto bad;
    for (i = 0; i < compiled->sources->package_count; i++) if (sh_package_owners_contains(&owners, i)) {
        const sh_package_sources *sources = compiled->sources;
        size_t j;
        failure = "The compiled package ownership inventory is inconsistent.";
        if (count == capacity) goto bad;
        for (j = 0; j < sources->component_count; j++) if (sources->components[j].owner == i && !sources->components[j].relative[0]) break;
        failure = "A supplying package has no retained complete authored delivery.";
        if (j == sources->component_count || !sources->fingerprints ||
            strcpy_s(selected[count].name, sizeof(selected[count].name), sources->components[j].descriptor.name) ||
            !(selected[count].bundle = sh_package_runtime_source_bundle(i, sources->fingerprints[i]))) goto bad;
        sh_package_bundle_delivery_key(selected[count].bundle, selected[count].id);
        for (j = 0; j < count; j++) if (!strcmp(selected[j].id, selected[count].id)) break;
        if (j < count) { /* Identical complete copies need one carrier. */
            sh_package_bundle_release(selected[count].bundle); memset(selected + count, 0, sizeof(*selected));
            continue;
        }
        count++;
    }
    sh_package_runtime_release();
    sh_package_references_free(&references);
    {
        char message[192];
        snprintf(message, sizeof(message), "MPKG: selected %zu complete authored packages; recorded dependency phases %s",
                 count, dependencies_complete ? "observed" : "partial");
        backend_log(message);
    }
    *out = selected; sh_package_owners_free(&owners); return count;
bad:
    if (error && error_capacity) snprintf(error, error_capacity, "%s", failure);
    sh_package_runtime_release(); sh_package_references_free(&references);
    sh_mpkg_used_free(selected); sh_package_owners_free(&owners); sh_package_provenance_free(bindings); return SIZE_MAX;
}

size_t sh_mpkg_used_packages(const char *json, size_t len, const char *data_root,
    sh_mpkg_used **out, char *error, size_t error_capacity)
{ return sh_mpkg_used_packages_with_bindings(json, len, data_root, out, NULL, error, error_capacity); }
