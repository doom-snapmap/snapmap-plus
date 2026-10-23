#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_document.h"
#include "map_provenance.h"
#include "map_embed.h"
#include "package_occurrence.h"
#include "package_link_inputs.h"
#include "resource_catalog.h"
#include "package_sources.h"
#include "backend_log.h"
#include "package_installed_open.h"

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

static sh_package_document_stats g_document_stats;
void sh_package_document_bind_stats(sh_package_document_stats *out) { if (out) *out = g_document_stats; }
static unsigned long long pd_now(void) { return GetTickCount64(); }
int sh_package_document_bind(const char *root, const sh_mpkg_context *sources,
    char **json, size_t *length, sh_package_map_plan **out, char *error, size_t capacity)
{
    unsigned long long phase = pd_now();
    memset(&g_document_stats, 0, sizeof(g_document_stats));
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
        bound = sh_package_runtime_prepare_local_document(root, *json, *length,
            &bound_length, &plan, error, capacity);
        g_document_stats.prepare_ms = pd_now() - phase;
        if (!bound) goto done;
        HeapFree(GetProcessHeap(), 0, *json); *json = bound; *length = bound_length;
        *out = plan; plan = NULL; ok = 1; goto done;
    }
    {
        /* R-401: every carried delivery installed and every recorded identity the library's: no map context. */
        int installed = sh_package_installed_open(root, sources, present ? &saved : NULL, json, length, &plan, error, capacity);
        if (installed) {
            g_document_stats.prepare_ms = pd_now() - phase; phase = pd_now();
            int native = pd_native(plan, present ? &saved : NULL, json, length, error, capacity);
            g_document_stats.native_ms = pd_now() - phase;
            if (!native) goto done;
            *out = plan; plan = NULL; ok = 1; goto done;
        }
    }
    size_t bundle_count = 0;
    const sh_package_bundle_binding *bundles = sh_mpkg_context_bundles(sources, &bundle_count);
    plan = sh_package_runtime_prepare_map_document(root, sh_mpkg_context_root(sources),
        present ? &saved : NULL, bundles, bundle_count, *json, *length, error, capacity);
    g_document_stats.prepare_ms = pd_now() - phase; phase = pd_now();
    if (!plan) goto done;
    bound = sh_package_map_plan_rebind(plan, present ? &saved : NULL,
        *json, *length, &bound_length, error, capacity);
    g_document_stats.rebind_ms = pd_now() - phase; phase = pd_now();
    if (!bound) goto done;
    HeapFree(GetProcessHeap(), 0, *json); *json = bound; *length = bound_length;
    int native = pd_native(plan, present ? &saved : NULL, json, length, error, capacity);
    g_document_stats.native_ms = pd_now() - phase;
    if (!native) goto done;
    *out = plan; plan = NULL; ok = 1;
done:
    sh_package_map_plan_free(plan); sh_package_provenance_free(&saved);
    return ok;
}

int sh_package_document_stock(void *context, const char *family, const char *name)
{
    char *path;
    unsigned char *body = NULL;
    size_t length = 0;
    int status = -1;
    sh_package_baseline_reader reader = NULL;
    void *reader_context = NULL;
    (void)context;
    if (!family || !name) return -1;
    /* Asset families cook to their own paths; a declaration family reads
     * generated/decls/<family>/<name>.decl, as the source graph does. */
    path = sh_package_cooked_path(family, name);
    if (!path) {
        size_t capacity = strlen(family) + strlen(name) + 24;
        char *raw = malloc(capacity);
        if (!raw) return -1;
        snprintf(raw, capacity, "generated/decls/%s/%s.decl", family, name);
        path = sh_package_engine_path(raw); free(raw);
        if (!path) return -1;
    }
    sh_package_runtime_acquire();
    {
        const sh_resource_catalog *catalog;
#ifdef SH_PACKAGE_RUNTIME_TESTING
        /* A test baseline stands for the game's resources when one is set. */
        sh_package_runtime_test_source_inputs(NULL, NULL, &reader, &reader_context);
#endif
        if (!reader && (catalog = sh_package_runtime_catalog()) != NULL) {
            reader = sh_resource_catalog_read_path; reader_context = (void *)catalog;
        }
        if (reader) status = reader(reader_context, path, &body, &length);
    }
    sh_package_runtime_release();
    free(body); free(path);
    return status < 0 ? -1 : status != 0;
}
