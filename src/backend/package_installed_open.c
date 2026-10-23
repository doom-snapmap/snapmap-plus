#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_installed_open.h"
#include "package_runtime.h"
#include "package_compiler.h"
#include "package_bundle.h"
#include "backend_log.h"

/* R-401 step 2: the open of a map every carried delivery of which is an installed package tree. Its package rows are
 * the library's: a binding record names content-addressed private identities (smp/ + the binding digest), and the
 * library compiled the same trees. What a map context compile adds over the library is the document's own R-255
 * variant rows and copies of installed originals (shader includes), neither of which needs the compile. So when
 * every delivery tree is installed and every identity the record names is served by the library, the document is
 * bound as a local document over the library (sh_package_runtime_prepare_local_document), whose plan holds only the
 * document's variant rows, and no map context compiles. Anything else returns 0 and the open compiles as before. */
static int pio_library_serves(const sh_package_compilation *library, const char *family, const char *name)
{
    for (size_t i = 0; library && i < library->resource_count; i++)
        if (sh_package_resource_native_matches(library->resources + i, family, name)) return 1;
    return 0;
}

int sh_package_installed_open(const char *root, const sh_mpkg_context *sources, const sh_package_provenance *saved,
    char **json, size_t *length, sh_package_map_plan **out, char *error, size_t capacity)
{
    size_t count = 0, unserved = 0, checked = 0;
    const sh_package_bundle_binding *bundles;
    const char *first = NULL;
    char line[512], reason[512] = "";
    *out = NULL;
    char off[4] = "";
    /* SNAPMAP_PLUS_INSTALLED_OPEN=0 keeps the map context compile (a comparison switch). */
    if (GetEnvironmentVariableA("SNAPMAP_PLUS_INSTALLED_OPEN", off, sizeof(off)) == 1 && off[0] == '0') return 0;
    if (!sh_package_runtime_register_once_enabled() || !sources || !saved || !saved->binding_count || !json || !*json)
        return 0;
    bundles = sh_mpkg_context_bundles(sources, &count);
    if (!count) return 0;
    for (size_t i = 0; i < count; i++) {
        sh_package_bundle *installed = sh_package_runtime_installed_bundle(bundles[i].compiler_tree);
        if (!installed) return 0; /* a delivery the library does not hold: the map context compiles */
        sh_package_bundle_release(installed);
    }
    {
        const sh_package_compilation *library = sh_package_runtime_library_acquire();
        for (size_t i = 0; i < saved->binding_count; i++) {
            const sh_package_provenance_binding *binding = saved->bindings + i;
            if (!binding->family || !binding->name) continue;
            checked++;
            if (!pio_library_serves(library, binding->family, binding->name)) { if (!unserved++) first = binding->name; }
        }
        if (unserved) snprintf(reason, sizeof(reason), "%zu of %zu recorded binding(s) are not the library's (first: %.200s)",
            unserved, checked, first ? first : "?");
        sh_package_runtime_release();
    }
    if (unserved) {
        snprintf(line, sizeof(line), "MPKG: installed open not taken: every carried delivery is installed, but %s; "
            "the map context compiles", reason);
        backend_log(line);
        return 0;
    }
    size_t bound_length = 0;
    sh_package_map_plan *plan = NULL;
    char *bound = sh_package_runtime_prepare_local_document(root, *json, *length, &bound_length, &plan, error, capacity);
    if (!bound) {
        snprintf(line, sizeof(line), "MPKG: installed open not taken: the document could not bind over the library (%.300s); "
            "the map context compiles", error && *error ? error : "no detail");
        backend_log(line);
        if (error && capacity) *error = 0;
        return 0;
    }
    HeapFree(GetProcessHeap(), 0, *json); *json = bound; *length = bound_length;
    snprintf(line, sizeof(line), "MPKG: installed open: %zu carried deliver%s installed and %zu recorded binding(s) served "
        "by the library; the map binds over the registered library with %s, no map context compiles", count,
        count == 1 ? "y is" : "ies are", checked, plan ? "its document's variant rows" : "no rows of its own");
    backend_log(line);
    *out = plan;
    return 1;
}
