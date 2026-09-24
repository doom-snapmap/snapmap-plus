/* Pure typed-use classification and intact delivery selection. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "map_embed.h"

void sh_mpkg_used_free(sh_mpkg_used *used)
{
    if (used) for (size_t i = 0; used[i].bundle; i++) sh_package_bundle_release(used[i].bundle);
    free(used);
}

static int embed_gameplay_required(const sh_compiled_resource *resource)
{
    return resource && (sh_package_owners_count(&resource->gameplay_owners) ||
        (resource->original_path && strcmp(resource->original_path, resource->engine_path)) ||
        (resource->baseline_known == 2 && resource->source_count));
}
int sh_mpkg_new_use_required(void *context, const char *family, const char *name,
    char *error, size_t capacity)
{
    const sh_mpkg_use_requirements *requirements = context;
    const sh_resource_catalog_entry *const *entries = NULL;
    char *path;
    size_t a, b, count;
    if (error && capacity) *error = 0;
    if (!requirements || !requirements->compiled || !family || !name) {
        if (error && capacity) snprintf(error, capacity, "current map gameplay classification is unavailable");
        return -1;
    }
    const sh_package_compilation *compiled = requirements->compiled;
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = compiled->resources + i;
        if (sh_package_resource_native_matches(resource, family, name) && embed_gameplay_required(resource)) return 1;
    }
    a = strlen(family); b = strlen(name);
    if (a > SIZE_MAX - 23 || b > SIZE_MAX - a - 23 || !(path = malloc(a + b + 23))) {
        if (error && capacity) snprintf(error, capacity, "cannot inspect a current map resource identity"); return -1;
    }
    snprintf(path, a + b + 23, "generated/decls/%s/%s.decl", family, name);
    int needed = embed_gameplay_required(sh_package_compilation_find(compiled, path)); free(path);
    if (needed) return 1;
    count = sh_resource_catalog_find(requirements->catalog, family, name, &entries);
    for (size_t i = 0; i < count; i++) if (entries[i]->path[0] &&
        embed_gameplay_required(sh_package_compilation_find(compiled, entries[i]->path))) return 1;
    return 0;
}

size_t sh_mpkg_used_packages_for_uses(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, const char *json, size_t length,
    const sh_package_references *references, const sh_package_provenance *uses,
    const sh_package_bundle_binding *bundles, size_t bundle_count,
    sh_mpkg_used **out, sh_package_references *resources, char *error, size_t capacity)
{
    sh_package_provenance selected = {0};
    sh_mpkg_used *deliveries = NULL;
    size_t count = 0;
    const char *failure = "Explicit map occurrence outputs are unavailable.";
    if (error && capacity) *error = 0;
    if (!out || !resources || resources == references) goto bad;
    sh_mpkg_used_free(*out); *out = NULL; sh_package_references_free(resources);
    failure = "Explicit map occurrence ownership is unavailable.";
    if (!compiled || !uses || uses->semantics == SH_PACKAGE_PROVENANCE_CATALOG ||
        (bundle_count && !bundles) || !sh_package_provenance_select_uses(uses, NULL, NULL, &selected)) goto bad;
    failure = "Map gameplay resource traversal failed.";
    if (!sh_package_map_resources(compiled, catalog, json, length, references, resources)) goto bad;
    /* This checks captured compiled payloads, independently of which exact
     * equal source the occurrences selected. Dependency coverage stays visible. */
    for (size_t i = 0; i < resources->count; i++) {
        const char *path = resources->items[i].name;
        if (sh_package_compilation_find(compiled, path) &&
            sh_package_compilation_probe(compiled, path, error, capacity) != 1) {
            failure = "A required compiled map resource is unavailable."; goto bad;
        }
    }
    failure = "Cannot allocate retained map deliveries.";
    if (bundle_count >= SIZE_MAX / sizeof(*deliveries) ||
        (bundle_count && !(deliveries = calloc(bundle_count + 1u, sizeof(*deliveries))))) goto bad;
    for (size_t c = 0; c < selected.context_count; c++) {
        const sh_package_provenance_context *context = selected.contexts + c;
        for (size_t t = 0; t < context->tree_count; t++) {
            int found = 0;
            for (size_t i = 0; i < bundle_count; i++) {
                const unsigned char *tree = sh_package_bundle_fingerprint(bundles[i].original);
                char key[SH_PACKAGE_BUNDLE_KEY_CAP]; size_t d;
                if (!tree || memcmp(tree, context->trees[t], 32)) continue;
                found = 1; sh_package_bundle_delivery_key(bundles[i].original, key);
                for (d = 0; d < count; d++) if (!strcmp(deliveries[d].id, key)) break;
                if (d < count) continue;
                failure = "A selected map delivery has invalid retained metadata.";
                if (count == bundle_count || !memchr(bundles[i].source_name, 0, sizeof(bundles[i].source_name)) ||
                    strcpy_s(deliveries[count].id, sizeof(deliveries[count].id), key) ||
                    strcpy_s(deliveries[count].name, sizeof(deliveries[count].name),
                        bundles[i].source_name[0] ? bundles[i].source_name : "Retained source")) goto bad;
                deliveries[count++].bundle = sh_package_bundle_retain(bundles[i].original);
            }
            failure = "A selected map source has no exact retained original delivery.";
            if (!found) goto bad;
        }
    }
    *out = deliveries; sh_package_provenance_free(&selected); return count;
bad:
    if (error && capacity && !*error) snprintf(error, capacity, "%s", failure);
    sh_mpkg_used_free(deliveries); sh_package_provenance_free(&selected);
    if (out) { sh_mpkg_used_free(*out); *out = NULL; }
    if (resources && resources != references) sh_package_references_free(resources);
    return SIZE_MAX;
}
