#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_document.h"
#include "map_provenance.h"
#include "map_embed.h"
#include "package_occurrence.h"
#include "backend_log.h"

/* Both saved-map and retained-request routes compile the same authored context.
 * Only a document without delivery or ownership can resolve historical custom
 * names from the local library. Saved authored contexts remain independent. */
/* Before availability, consent or bindings are decided, represent exactly
 * convertible package uses as native map data (the save path's mechanism).
 * The embedded deliveries stay the map's retained sources, unchanged. A failed
 * representation keeps the package route; only an operational error stops. */
static int pd_native(sh_package_map_plan *plan, const sh_package_provenance *saved,
    char **json, size_t *length, char *error, size_t capacity)
{
    char *derived = NULL, *report = NULL, inline_error[512] = "";
    size_t derived_length = 0, count = 0;
    sh_package_occurrence_snapshot *ownership = NULL;
    sh_mpkg_used *used = NULL;
    int result = sh_package_runtime_native_document(plan, saved, NULL, 0, *json, *length,
        &derived, &derived_length, &ownership, &used, &count, &report, inline_error, sizeof(inline_error));
    for (char *line = report, *end; line && *line; line = end + 1) {
        char entry[768];
        end = strchr(line, '\n'); if (!end) end = line + strlen(line);
        snprintf(entry, sizeof(entry), "MPKG: native map data (load): %.*s", (int)(end - line), line);
        backend_log(entry);
        if (!*end) break;
    }
    free(report); sh_package_occurrence_release(ownership); sh_mpkg_used_free(used);
    if (result < 0 && inline_error[0]) {
        char entry[640];
        snprintf(entry, sizeof(entry), "MPKG: native map data unavailable at load; keeping package routes (%s)", inline_error);
        backend_log(entry);
    }
    if (result == 1) {
        char *heap = HeapAlloc(GetProcessHeap(), 0, derived_length + 1);
        if (!heap) { free(derived); if (error && capacity) snprintf(error, capacity, "Map source allocation failed."); return 0; }
        memcpy(heap, derived, derived_length); heap[derived_length] = 0;
        HeapFree(GetProcessHeap(), 0, *json); *json = heap; *length = derived_length;
    }
    free(derived);
    return 1;
}

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
    if (!pd_native(plan, present ? &saved : NULL, json, length, error, capacity)) goto done;
    *out = plan; plan = NULL; ok = 1;
done:
    sh_package_map_plan_free(plan); sh_package_provenance_free(&saved);
    return ok;
}
