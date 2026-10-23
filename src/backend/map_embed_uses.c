/* Pure typed-use classification and intact delivery selection. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "map_embed.h"
#include "package_keys.h"

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

/* The row whose exact original tree is `tree`, or bundle_count. */
static size_t embed_tree_bundle(const sh_package_bundle_binding *bundles, size_t bundle_count, const unsigned char tree[32])
{
    for (size_t i = 0; i < bundle_count; i++) {
        const unsigned char *candidate = sh_package_bundle_fingerprint(bundles[i].original);
        if (candidate && !memcmp(candidate, tree, 32)) return i;
    }
    return bundle_count;
}

/* How a composed context relates to the resource it is used for. The current
 * compile's source files of that resource, restricted to owners whose trees
 * the context names, are its contributors. When every tree of the context
 * contributes, the composition is the resource's own (a private copy built
 * from several packages) and all of it is required: 0. When some tree
 * contributes nothing, the record composed more than this use needs (a map
 * saved before binding records composes every carried package): with one
 * contributing component it is that component's, 2, with row and component
 * set; with several it is a shared copy, 1, and the deliveries the map's other
 * uses select decide, as they do for a use with no compiled resource (a
 * registration key), one none of the composed trees supplies, or a stock
 * identity every tree contributes a copy of. */
static int embed_composed_supplier(const sh_package_compilation *compiled, const sh_package_bundle_binding *bundles,
    size_t bundle_count, const sh_package_provenance_context *context, const char *family, const char *name,
    size_t *row, const char **component)
{
    const sh_package_sources *sources = compiled ? compiled->sources : NULL;
    const sh_compiled_resource *resource = NULL;
    size_t owner = SIZE_MAX, part = SIZE_MAX, found_row = bundle_count, pairs = 0;
    unsigned char *contributes;
    int wider = 0;
    if (!sources || !family || !name || !context->tree_count) return 0;
    for (size_t i = 0; i < compiled->resource_count && !resource; i++)
        if (sh_package_resource_native_matches(compiled->resources + i, family, name)) resource = compiled->resources + i;
    if (!resource) {
        size_t a = strlen(family), b = strlen(name);
        char *path;
        if (a > SIZE_MAX - 23 || b > SIZE_MAX - a - 23 || !(path = malloc(a + b + 23))) return 0;
        snprintf(path, a + b + 23, "generated/decls/%s/%s.decl", family, name);
        resource = sh_package_compilation_find(compiled, path); free(path);
    }
    if (!resource) return 1;   /* no compiled resource (a registration key): the other uses decide */
    if (!(contributes = calloc(context->tree_count, 1))) return 0;
    for (size_t s = 0; s < resource->source_count; s++) {
        const sh_package_source_file *file;
        size_t t, i;
        if (resource->sources[s] >= sources->file_count) { free(contributes); return 0; }
        file = sources->files + resource->sources[s];
        if (file->owner >= sources->package_count || !sources->fingerprints) { free(contributes); return 0; }
        for (i = 0; i < bundle_count; i++)
            if (!memcmp(bundles[i].compiler_tree, sources->fingerprints[file->owner], 32)) break;
        if (i == bundle_count) continue;
        for (t = 0; t < context->tree_count; t++) {
            const unsigned char *tree = sh_package_bundle_fingerprint(bundles[i].original);
            if (tree && !memcmp(tree, context->trees[t], 32)) break;
        }
        if (t == context->tree_count) continue;   /* a copy the record did not compose */
        contributes[t] = 1;
        if (owner == SIZE_MAX || owner != file->owner || part != file->component) {
            pairs++; owner = file->owner; part = file->component; found_row = i;
        }
    }
    for (size_t t = 0; t < context->tree_count; t++) if (!contributes[t]) wider = 1;
    free(contributes);
    if (!pairs) return 1;      /* nothing the record composed supplies it: the other uses decide */
    /* Every tree contributes: the resource's own composition. For an identity
     * the game already has (a stock registration list every contributor ships
     * a copy of) the component the map uses carries its copy, so the other
     * uses decide; an added identity needs every contributor. */
    if (!wider) return resource->baseline_known == 1 || resource->baseline_known == 3 ? 1 : 0;
    if (pairs > 1 || part >= sources->component_count) return 1;
    *row = found_row; *component = sources->components[part].relative ? sources->components[part].relative : "";
    return 2;
}

/* One whole delivery of a bundle, deduplicated by key. */
static int embed_whole_delivery(const sh_package_bundle_binding *row, sh_mpkg_used *deliveries, size_t *count)
{
    char key[SH_PACKAGE_BUNDLE_KEY_CAP];
    size_t d;
    sh_package_bundle_delivery_key(row->original, key);
    for (d = 0; d < *count; d++) if (!strcmp(deliveries[d].id, key)) {
        /* A whole delivery supersedes component deliveries of the same tree. */
        return 1;
    }
    if (!memchr(row->source_name, 0, sizeof(row->source_name)) ||
        strcpy_s(deliveries[*count].id, sizeof(deliveries[*count].id), key) ||
        strcpy_s(deliveries[*count].name, sizeof(deliveries[*count].name),
            row->source_name[0] ? row->source_name : "Retained source")) return 0;
    deliveries[*count].nested = 0;
    deliveries[(*count)++].bundle = sh_package_bundle_retain(row->original);
    return 1;
}

/* The compiler owner whose inventoried tree is this row's. */
static size_t embed_row_owner(const sh_package_compilation *compiled, const sh_package_bundle_binding *row)
{
    const sh_package_sources *sources = compiled->sources;
    if (!sources || !sources->fingerprints) return SIZE_MAX;
    for (size_t p = 0; p < sources->package_count; p++)
        if (!memcmp(sources->fingerprints[p], row->compiler_tree, 32)) return p;
    for (size_t p = 0; p < sources->package_count; p++)
        if (row->source_name[0] && !strcmp(sources->packages[p].name, row->source_name)) return p;
    return SIZE_MAX;
}

/* The enclosing files a component's closure resolves to: every source file of
 * a resolved resource that belongs to the same owner but lies outside the
 * component. Unique, in closure order. */
int sh_mpkg_component_enclosing_files(const sh_package_compilation *compiled, size_t owner, const char *component,
    const sh_package_references *resources, char ***out, size_t *count)
{
    const sh_package_sources *sources = compiled->sources;
    size_t prefix = strlen(component);
    *out = NULL; *count = 0;
    for (size_t r = 0; r < resources->count; r++) {
        const sh_compiled_resource *resource = sh_package_compilation_find(compiled, resources->items[r].name);
        if (!resource) continue;
        for (size_t s = 0; s < resource->source_count; s++) {
            const sh_package_source_file *file;
            char **grown;
            size_t k;
            if (resource->sources[s] >= sources->file_count) continue;
            file = sources->files + resource->sources[s];
            if (file->owner != owner || file->directory || !file->relative) continue;
            if (!strncmp(file->relative, component, prefix) && file->relative[prefix] == '/') continue;
            for (k = 0; k < *count; k++) if (!strcmp((*out)[k], file->relative)) break;
            if (k < *count) continue;
            if (*count >= SIZE_MAX / sizeof(*grown) - 1 || !(grown = realloc(*out, (*count + 1) * sizeof(*grown)))) return 0;
            *out = grown;
            if (!((*out)[*count] = _strdup(file->relative))) return 0;
            (*count)++;
        }
    }
    return 1;
}

void sh_mpkg_strings_free(char **items, size_t count)
{
    for (size_t i = 0; i < count; i++) free(items[i]);
    free(items);
}

/* A whole or component delivery for one selected context tree. A component
 * delivery that cannot be built (no retained cache root, an unknown owner or
 * component, or a refused closure file) falls back to the whole tree, which is
 * always a valid delivery, and the log says why. */
static int embed_select_delivery(const sh_package_compilation *compiled, const sh_package_bundle_binding *row,
    const char *component, const sh_package_references *resources, sh_mpkg_used *deliveries, size_t *count)
{
    const sh_package_sources *sources = compiled->sources;
    const char *root = sh_package_bundle_cache_root(), *why = NULL;
    const unsigned char *source_tree = sh_package_bundle_fingerprint(row->original);
    sh_package_bundle *delivery = NULL;
    char **enclosing = NULL, key[SH_PACKAGE_BUNDLE_KEY_CAP], error[512] = "", line[1024];
    size_t enclosing_count = 0, owner, c, d;
    if (!component || !component[0] || strlen(component) >= SH_MPKG_COMPONENT_CAP)
        return embed_whole_delivery(row, deliveries, count);
    /* The same tree already travelling whole covers every component. */
    sh_package_bundle_delivery_key(row->original, key);
    for (d = 0; d < *count; d++) if (!strcmp(deliveries[d].id, key)) return 1;
    for (d = 0; d < *count; d++)
        if (deliveries[d].nested && !memcmp(deliveries[d].source_tree, source_tree, 32) &&
            !strcmp(deliveries[d].component, component)) return 1;
    owner = embed_row_owner(compiled, row);
    if (!root) why = "no retained delivery cache root";
    else if (owner == SIZE_MAX || !sources) why = "the delivery's compiler owner is unknown";
    else {
        for (c = 0; c < sources->component_count; c++)
            if (sources->components[c].owner == owner && sources->components[c].relative &&
                !strcmp(sources->components[c].relative, component)) break;
        if (c == sources->component_count) why = "the component is not in the compiler inventory";
        else if (!sh_mpkg_component_enclosing_files(compiled, owner, component, resources, &enclosing, &enclosing_count))
            why = "cannot list the component's enclosing files";
        else if (!(delivery = sh_package_bundle_component(row->original, component,
                (const char *const *)enclosing, enclosing_count, root, error, sizeof(error)))) why = error;
    }
    if (!delivery) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "MPKG: component '%s' travels as its whole enclosing tree (%s)", component, why ? why : "unknown");
        backend_log(line);
        sh_mpkg_strings_free(enclosing, enclosing_count);
        return embed_whole_delivery(row, deliveries, count);
    }
    sh_package_bundle_delivery_key(delivery, deliveries[*count].id);
    strcpy_s(deliveries[*count].name, sizeof(deliveries[*count].name),
        sources->components[c].descriptor.name[0] ? sources->components[c].descriptor.name : component);
    deliveries[*count].nested = 1;
    memcpy(deliveries[*count].source_tree, source_tree, 32);
    strcpy_s(deliveries[*count].component, sizeof(deliveries[*count].component), component);
    deliveries[*count].bundle = delivery;
    (*count)++;
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "MPKG: component '%s' of '%s' travels as its own delivery with %zu enclosing file(s) (%s)",
                component, sources->packages[owner].name, enclosing_count, deliveries[*count - 1].id);
    backend_log(line);
    sh_mpkg_strings_free(enclosing, enclosing_count);
    return 1;
}

size_t sh_mpkg_used_packages_for_uses(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, const char *json, size_t length,
    const sh_package_references *references, const sh_package_provenance *uses,
    const sh_package_bundle_binding *bundles, size_t bundle_count,
    sh_mpkg_used **out, sh_package_references *resources, char *error, size_t capacity)
{
    sh_package_provenance selected = {0};
    sh_mpkg_used *deliveries = NULL;
    struct { size_t bundle; const char *component; } *parts = NULL;
    int *whole = NULL;
    unsigned char *composed = NULL;
    size_t count = 0, part_count = 0;
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
    /* What each use requires. An ISOLATED context names the tree and the
     * component that supply the use; a tree used from its root travels whole,
     * one used only through nested components travels as one component
     * delivery per component (D4), and a whole tree covers its components. A
     * COMPOSED context is a shared contribution by several packages to one
     * resource, such as the AI registration list every component ships its own
     * copy of. When the map's own uses already select one of its contributors
     * (a component or whole tree), that delivery carries its copy and the
     * composition adds nothing; otherwise every contributor's whole tree
     * travels, as before. A historical (SMPBIND0) use keeps its inseparable
     * composition as whole trees, which is what that record meant. */
    failure = "Cannot allocate retained map deliveries.";
    {
        size_t slots = 0;
        for (size_t c = 0; c < selected.context_count; c++) slots += selected.contexts[c].tree_count;
        if (slots >= SIZE_MAX / sizeof(*deliveries) || bundle_count >= SIZE_MAX / sizeof(*whole) ||
            !(deliveries = calloc(slots + bundle_count + 1u, sizeof(*deliveries))) ||
            !(whole = calloc(bundle_count + 1u, sizeof(*whole))) ||
            !(parts = calloc(slots + selected.use_count + 1u, sizeof(*parts))) ||
            !(composed = calloc(selected.context_count + 1u, sizeof(*composed)))) goto bad;
    }
    for (size_t u = 0; u < selected.use_count; u++) {
        const sh_package_provenance_use *use = selected.uses + u;
        int historical = use->kind == SH_PACKAGE_USE_HISTORICAL, authored;
        /* A use whose saved name is its authored name is reproduced by any
         * compile of the package that supplies it. A private name (a variant
         * or generated identity) exists only in the compile that made it, so
         * its context must travel exactly as recorded: whole, never split or
         * narrowed (correctness over size). */
        failure = "A selected map use names an invalid source binding.";
        if (use->binding >= selected.binding_count) goto bad;
        authored = (selected.bindings[use->binding].original && selected.bindings[use->binding].original[0] &&
            selected.bindings[use->binding].name && !strcmp(selected.bindings[use->binding].original, selected.bindings[use->binding].name)) ||
            /* A registration key is relocated by every compile and rebound by the catalog alone. */
            (selected.bindings[use->binding].family && !strcmp(selected.bindings[use->binding].family, SH_PACKAGE_KEY_FAMILY));
        for (size_t k = 0; k < use->context_count; k++) {
            const sh_package_provenance_context *context;
            failure = "A selected map use names an invalid source context.";
            if (use->contexts[k] >= selected.context_count) goto bad;
            context = selected.contexts + use->contexts[k];
            if (!historical && !authored && context->mode == SH_PACKAGE_CONTEXT_COMPOSED) { composed[use->contexts[k]] = 2; continue; }
            if (!historical && context->mode == SH_PACKAGE_CONTEXT_COMPOSED) {
                /* A record that composed every carried package (a map saved
                 * before binding records) still names, through the current
                 * compile, the one component that supplies this use. */
                size_t row = bundle_count;
                const char *supplier = NULL;
                int relation = use->binding < selected.binding_count ? embed_composed_supplier(compiled, bundles,
                    bundle_count, context, selected.bindings[use->binding].family, selected.bindings[use->binding].name,
                    &row, &supplier) : 0;
                if (relation == 2) {
                    if (!supplier[0]) whole[row] = 1;
                    else {
                        size_t d;
                        for (d = 0; d < part_count; d++)
                            if (parts[d].bundle == row && !strcmp(parts[d].component, supplier)) break;
                        if (d == part_count) { parts[part_count].bundle = row; parts[part_count++].component = supplier; }
                    }
                }
                /* 2: the composition's own requirement is covered by its one
                 * supplier; 1: the map's other uses decide; 0: all of it. */
                if (composed[use->contexts[k]] != 2) composed[use->contexts[k]] = relation ? 1 : 2;
                continue;
            }
            for (size_t t = 0; t < context->tree_count; t++) {
                size_t i = embed_tree_bundle(bundles, bundle_count, context->trees[t]);
                failure = "A selected map source has no exact retained original delivery.";
                if (i == bundle_count) goto bad;
                if (historical || !authored || !context->component || !context->component[0]) { whole[i] = 1; continue; }
                {
                    size_t d;
                    for (d = 0; d < part_count; d++)
                        if (parts[d].bundle == i && !strcmp(parts[d].component, context->component)) break;
                    if (d == part_count) { parts[part_count].bundle = i; parts[part_count++].component = context->component; }
                }
            }
        }
    }
    for (size_t c = 0; c < selected.context_count; c++) {
        const sh_package_provenance_context *context = selected.contexts + c;
        int covered = 0;
        if (!composed[c]) continue;
        for (size_t t = 0; composed[c] == 1 && t < context->tree_count && !covered; t++) {
            size_t i = embed_tree_bundle(bundles, bundle_count, context->trees[t]);
            if (i == bundle_count) continue;
            if (whole[i]) covered = 1;
            for (size_t d = 0; d < part_count && !covered; d++) if (parts[d].bundle == i) covered = 1;
        }
        if (covered) {
            char line[192];
            _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: a shared contribution composed from %zu package tree(s) "
                        "follows the delivery the map's own uses select; it adds no delivery of its own", context->tree_count);
            backend_log(line);
            continue;
        }
        for (size_t t = 0; t < context->tree_count; t++) {
            size_t i = embed_tree_bundle(bundles, bundle_count, context->trees[t]);
            failure = "A selected map source has no exact retained original delivery.";
            if (i == bundle_count) goto bad;
            whole[i] = 1;
        }
    }
    failure = "A selected map delivery has invalid retained metadata.";
    for (size_t i = 0; i < bundle_count; i++)
        if (whole[i] && !embed_whole_delivery(bundles + i, deliveries, &count)) goto bad;
    for (size_t d = 0; d < part_count; d++)
        if (!whole[parts[d].bundle] && !embed_select_delivery(compiled, bundles + parts[d].bundle, parts[d].component,
            resources, deliveries, &count)) goto bad;
    free(whole); free(parts); free(composed);
    *out = deliveries; sh_package_provenance_free(&selected); return count;
bad:
    if (error && capacity && !*error) snprintf(error, capacity, "%s", failure);
    free(whole); free(parts); free(composed);
    sh_mpkg_used_free(deliveries); sh_package_provenance_free(&selected);
    if (out) { sh_mpkg_used_free(*out); *out = NULL; }
    if (resources && resources != references) sh_package_references_free(resources);
    return SIZE_MAX;
}

/* Whether a saved context (tree, component) is what this delivery carries: the
 * whole tree, or the named component of the tree it was built from. */
static int embed_delivery_matches(const sh_mpkg_used *used, const unsigned char tree[32], const char *component)
{
    if (used->nested)
        return !memcmp(used->source_tree, tree, 32) && component && !strcmp(component, used->component);
    return used->bundle && !memcmp(sh_package_bundle_fingerprint(used->bundle), tree, 32);
}

void sh_mpkg_used_retarget_record(sh_package_provenance *record, const sh_mpkg_used *used, size_t count)
{
    size_t c, t, i;
    if (!record || !used) return;
    for (c = 0; c < record->context_count; c++) {
        sh_package_provenance_context *scope = record->contexts + c;
        for (t = 0; t < scope->tree_count; t++) for (i = 0; i < count; i++) {
            const unsigned char *tree;
            if (!used[i].nested || !embed_delivery_matches(used + i, scope->trees[t], scope->component)) continue;
            tree = sh_package_bundle_fingerprint(used[i].bundle);
            if (!tree) continue;
            memcpy(scope->trees[t], tree, 32);
            if (scope->component) scope->component[0] = 0;
            break;
        }
    }
}
