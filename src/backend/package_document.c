#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "package_document.h"
#include "map_provenance.h"

/* Both saved-map and retained-request routes compile the same authored context.
 * Only a document without delivery or ownership can resolve historical custom
 * names from the local library. Saved authored contexts remain independent. */
int sh_package_document_bind(const char *root, const sh_mpkg_context *sources,
    char **json, size_t *length, sh_package_map_plan **out, char *error, size_t capacity)
{
    sh_package_provenance saved = {0};
    sh_package_map_plan *plan = NULL;
    char *bound = NULL;
    size_t bound_length = 0;
    int present = 0, ok = 0;
    *out = NULL;
    if (!sh_map_provenance_read(*json, *length, &saved, &present, error, capacity)) goto done;
    if (!sh_mpkg_context_count(sources)) {
        if (saved.binding_count) {
            snprintf(error, capacity, "The map's authored source bindings have no package delivery.");
            goto done;
        }
        bound = sh_package_runtime_rebind_local_document(*json, *length,
            &bound_length, error, capacity);
        if (!bound) goto done;
        HeapFree(GetProcessHeap(), 0, *json); *json = bound; *length = bound_length;
        ok = 1; goto done;
    }
    size_t bundle_count = 0;
    const sh_package_bundle_binding *bundles = sh_mpkg_context_bundles(sources, &bundle_count);
    plan = sh_package_runtime_prepare_map_document(root, sh_mpkg_context_root(sources),
        present ? &saved : NULL, bundles, bundle_count, *json, *length, error, capacity);
    if (!plan) goto done;
    bound = sh_package_map_plan_rebind(plan, present ? &saved : NULL,
        *json, *length, &bound_length, error, capacity);
    if (!bound) goto done;
    HeapFree(GetProcessHeap(), 0, *json); *json = bound; *length = bound_length;
    *out = plan; plan = NULL; ok = 1;
done:
    sh_package_map_plan_free(plan); sh_package_provenance_free(&saved);
    return ok;
}
