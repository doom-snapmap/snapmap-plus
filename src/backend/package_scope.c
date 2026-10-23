#include "package_scope.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct sc_file { const char *path; size_t source, depth; } sc_file;

static int sc_ancestor(const sh_package_component *parent, const sh_package_component *child)
{
    size_t length;
    if (parent->owner != child->owner || !parent->relative || !child->relative) return 0;
    length = strlen(parent->relative);
    return !length || (!strncmp(parent->relative, child->relative, length) &&
        (!child->relative[length] || child->relative[length] == '/'));
}

static int sc_order(const void *left, const void *right)
{
    const sc_file *a = left, *b = right;
    int cmp = strcmp(a->path, b->path);
    if (cmp) return cmp;
    return a->depth > b->depth ? -1 : a->depth < b->depth;
}

void sh_package_scope_close(sh_package_scope *scope)
{
    if (!scope) return;
    free(scope->sources.components);
    if (!scope->files_shared) free(scope->sources.files);
    memset(scope, 0, sizeof(*scope));
}
void sh_package_scope_share_files(sh_package_scope *view, const sh_package_sources *sources)
{
    if (!view || !sources || view->files_shared || view->sources.file_count != sources->file_count ||
        (sources->file_count && !sources->files)) return;
    free(view->sources.files);
    view->sources.files = sources->files; view->files_shared = 1;
}

static size_t sc_find(const sh_package_sources *sources,
    const unsigned char fingerprint[32], const char *relative, size_t *indices, size_t capacity, int legacy)
{
    size_t count = 0;
    int original_match = 0;
    if (!sources || !fingerprint || !relative ||
        (sources->package_count && !sources->fingerprints) ||
        (sources->component_count && !sources->components)) return SIZE_MAX;
    /* Old carriers recorded their normalized tree. Prefer exact original
     * identity whenever available; use the normalized fallback only when no
     * original matches. All old matches survive, never first-match selection. */
    for (size_t i = 0; i < sources->package_count; i++)
        if (!memcmp(sh_package_source_tree(sources, i), fingerprint, 32)) original_match = 1;
    if (!original_match && !legacy) return 0;
    for (size_t i = 0; i < sources->component_count; i++) {
        const sh_package_component *component = sources->components + i;
        if (component->owner >= sources->package_count || !component->relative) return SIZE_MAX;
        if (!strcmp(component->relative, relative) &&
            !memcmp(original_match ? sh_package_source_tree(sources, component->owner) :
                sources->fingerprints[component->owner], fingerprint, 32)) count++;
    }
    if (!indices) return count;
    if (capacity < count) return SIZE_MAX;
    for (size_t i = 0, at = 0; i < sources->component_count; i++) {
        const sh_package_component *component = sources->components + i;
        if (!strcmp(component->relative, relative) &&
            !memcmp(original_match ? sh_package_source_tree(sources, component->owner) :
                sources->fingerprints[component->owner], fingerprint, 32)) indices[at++] = i;
    }
    return count;
}

size_t sh_package_scope_find(const sh_package_sources *sources,
    const unsigned char fingerprint[32], const char *relative, size_t *indices, size_t capacity)
{ return sc_find(sources, fingerprint, relative, indices, capacity, 0); }
size_t sh_package_scope_find_legacy(const sh_package_sources *sources,
    const unsigned char fingerprint[32], const char *relative, size_t *indices, size_t capacity)
{ return sc_find(sources, fingerprint, relative, indices, capacity, 1); }

int sh_package_scope_composed(const sh_package_sources *sources,
    const size_t *owners, size_t count, sh_package_scope *out, char *error, size_t capacity)
{
    sh_package_scope view = {0};
    unsigned char *selected = NULL;
    const char *reason = "composed package scope inventory is invalid";
    if (error && capacity) error[0] = 0;
    if (!out) return 0;
    sh_package_scope_close(out);
    if (!sources || (count && !owners) ||
        (sources->package_count && !sources->packages) ||
        (sources->component_count && !sources->components) ||
        (sources->file_count && !sources->files) ||
        sources->component_count > SIZE_MAX / sizeof(*view.sources.components) ||
        sources->file_count > SIZE_MAX / sizeof(*view.sources.files)) goto failed;
    view.sources = *sources; view.component = SIZE_MAX;
    view.sources.components = NULL; view.sources.files = NULL;
    selected = calloc(sources->package_count ? sources->package_count : 1, 1);
    view.sources.components = calloc(sources->component_count ? sources->component_count : 1,
        sizeof(*view.sources.components));
    view.sources.files = calloc(sources->file_count ? sources->file_count : 1, sizeof(*view.sources.files));
    if (!selected || !view.sources.components || !view.sources.files) {
        reason = "composed package scope allocation failed"; goto failed;
    }
    for (size_t i = 0; i < count; i++) {
        if (owners[i] >= sources->package_count) goto failed;
        selected[owners[i]] = 1;
    }
    for (size_t i = 0; i < sources->component_count; i++) {
        const sh_package_component *component = sources->components + i;
        if (component->owner >= sources->package_count || !component->relative) goto failed;
        view.sources.components[i] = *component;
        if (!selected[component->owner]) memset(&view.sources.components[i].descriptor, 0,
            sizeof(view.sources.components[i].descriptor));
    }
    for (size_t i = 0; i < sources->file_count; i++) {
        const sh_package_source_file *file = sources->files + i;
        if (file->owner >= sources->package_count || (file->engine_path && !file->directory &&
            (file->component >= sources->component_count ||
                sources->components[file->component].owner != file->owner))) goto failed;
        view.sources.files[i] = *file;
        if (!selected[file->owner]) view.sources.files[i].engine_path = NULL;
    }
    free(selected); *out = view; return 1;
failed:
    free(selected); sh_package_scope_close(&view);
    if (error && capacity) snprintf(error, capacity, "%s", reason);
    return 0;
}

typedef struct sc_tree_key { unsigned char fingerprint[32]; int found; } sc_tree_key;
static int sc_tree_order(const void *left, const void *right)
{ return memcmp(((const sc_tree_key *)left)->fingerprint, ((const sc_tree_key *)right)->fingerprint, 32); }

static int sc_composed_keys(const sh_package_sources *sources,
    const unsigned char (*fingerprints)[32], size_t count,
    sh_package_scope *out, char *error, size_t capacity, int legacy)
{
    sc_tree_key *keys = NULL;
    size_t *owners = NULL, unique = 0, selected = 0;
    int ok = 0;
    const char *reason = "composed source provenance is invalid";
    if (error && capacity) error[0] = 0;
    if (!out) return 0;
    sh_package_scope_close(out);
    if (!sources || (count && !fingerprints) ||
        (sources->package_count && !sources->fingerprints) ||
        count > SIZE_MAX / sizeof(*keys) || sources->package_count > SIZE_MAX / sizeof(*owners)) goto done;
    if (!count) return sh_package_scope_composed(sources, NULL, 0, out, error, capacity);
    keys = calloc(count, sizeof(*keys));
    owners = malloc((sources->package_count ? sources->package_count : 1) * sizeof(*owners));
    if (!keys || !owners) { reason = "cannot retain composed source provenance"; goto done; }
    for (size_t i = 0; i < count; i++) memcpy(keys[i].fingerprint, fingerprints[i], 32);
    qsort(keys, count, sizeof(*keys), sc_tree_order);
    for (size_t i = 0; i < count; i++)
        if (!unique || memcmp(keys[unique - 1].fingerprint, keys[i].fingerprint, 32)) keys[unique++] = keys[i];
    for (size_t i = 0; i < sources->package_count; i++) {
        sc_tree_key probe = {0}, *match;
        memcpy(probe.fingerprint, sh_package_source_tree(sources, i), 32);
        match = bsearch(&probe, keys, unique, sizeof(*keys), sc_tree_order);
        if (match) { match->found = 1; owners[selected++] = i; }
    }
    /* An old aggregate can contain normalized identities and new original
     * identities together. Fall back separately for each still-missing key. */
    for (size_t k = 0; legacy && k < unique; k++) if (!keys[k].found) {
        for (size_t i = 0; i < sources->package_count; i++) if (!memcmp(keys[k].fingerprint, sources->fingerprints[i], 32)) {
            size_t j = 0;
            while (j < selected && owners[j] != i) j++;
            if (j == selected) owners[selected++] = i;
            keys[k].found = 1;
        }
    }
    for (size_t i = 0; i < unique; i++) if (!keys[i].found) {
        reason = "an authored source tree for the composed map context is unavailable"; goto done;
    }
    ok = sh_package_scope_composed(sources, owners, selected, out, error, capacity);
    reason = NULL;
done:
    free(keys); free(owners);
    if (!ok && reason && error && capacity) snprintf(error, capacity, "%s", reason);
    return ok;
}

int sh_package_scope_composed_keys(const sh_package_sources *sources,
    const unsigned char (*fingerprints)[32], size_t count,
    sh_package_scope *out, char *error, size_t capacity)
{ return sc_composed_keys(sources, fingerprints, count, out, error, capacity, 0); }
int sh_package_scope_composed_keys_legacy(const sh_package_sources *sources,
    const unsigned char (*fingerprints)[32], size_t count,
    sh_package_scope *out, char *error, size_t capacity)
{ return sc_composed_keys(sources, fingerprints, count, out, error, capacity, 1); }

int sh_package_scope_open(const sh_package_sources *sources, size_t component,
    sh_package_scope *out, char *error, size_t capacity)
{
    sh_package_scope view = {0};
    sc_file *paths = NULL;
    size_t count = 0;
    const char *reason = "package scope inventory is invalid";
    if (error && capacity) error[0] = 0;
    if (!out) return 0;
    sh_package_scope_close(out);
    if (!sources || component >= sources->component_count || !sources->components ||
        (sources->file_count && !sources->files) || (sources->package_count && !sources->packages) ||
        sources->component_count > SIZE_MAX / sizeof(*view.sources.components) ||
        sources->file_count > SIZE_MAX / sizeof(*view.sources.files) ||
        sources->file_count > SIZE_MAX / sizeof(*paths)) goto failed;
    view.sources = *sources; view.component = component;
    view.sources.components = NULL; view.sources.files = NULL;
    if (sources->components[component].owner >= sources->package_count) goto failed;
    reason = "package scope allocation failed";
    view.sources.components = malloc(sources->component_count * sizeof(*view.sources.components));
    view.sources.files = calloc(sources->file_count ? sources->file_count : 1, sizeof(*view.sources.files));
    paths = calloc(sources->file_count ? sources->file_count : 1, sizeof(*paths));
    if (!view.sources.components || !view.sources.files || !paths) goto failed;
    memcpy(view.sources.components, sources->components, sources->component_count * sizeof(*view.sources.components));
    reason = "package scope ownership is invalid";
    for (size_t i = 0; i < sources->component_count; i++) {
        sh_package_component *entry = &view.sources.components[i];
        if (entry->owner >= sources->package_count || !entry->relative) goto failed;
        if (!sc_ancestor(entry, &sources->components[component]))
            memset(&entry->descriptor, 0, sizeof(entry->descriptor));
    }
    for (size_t i = 0; i < sources->file_count; i++) {
        const sh_package_source_file *file = &sources->files[i];
        view.sources.files[i] = *file; view.sources.files[i].engine_path = NULL;
        if (file->owner >= sources->package_count) goto failed;
        if (!file->engine_path || file->directory) continue;
        if (file->component >= sources->component_count ||
            sources->components[file->component].owner != file->owner) goto failed;
        if (!sc_ancestor(&sources->components[file->component], &sources->components[component])) continue;
        paths[count++] = (sc_file){file->engine_path, i, strlen(sources->components[file->component].relative)};
    }
    qsort(paths, count, sizeof(*paths), sc_order);
    for (size_t i = 0; i < count;) {
        size_t end = i + 1;
        while (end < count && !strcmp(paths[i].path, paths[end].path)) end++;
        if (end > i + 1 && paths[i].depth == paths[i + 1].depth) {
            reason = "one component supplies an ambiguous resource path"; goto failed;
        }
        view.sources.files[paths[i].source].engine_path = sources->files[paths[i].source].engine_path;
        i = end;
    }
    free(paths); *out = view; return 1;
failed:
    free(paths); sh_package_scope_close(&view);
    if (error && capacity) snprintf(error, capacity, "%s", reason);
    return 0;
}
