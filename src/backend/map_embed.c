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

static int embed_prepare_reference(void *context, const char *type, const char *name)
{
    if (!_stricmp(type, "entitydef"))
        sh_resource_graph_prepare_entity((sh_resource_graph_preparation *)context, name, NULL);
    return 1;
}
typedef struct embed_preparation {
    sh_resource_graph_preparation *native;
    sh_package_references *references;
} embed_preparation;

static int embed_prepare_state(void *context, const char *class_name, const char *inherit,
    const char *edit, size_t length)
{
    embed_preparation *preparation = context;
    sh_decl_dependency_result result = {0};
    if (!sh_resource_graph_prepare_inline(preparation->native, class_name, inherit, edit, length,
            sh_package_references_add, preparation->references, &result))
        preparation->references->incomplete = 1;
    return !result.aborted;
}

int sh_mpkg_prepare_map(const char *json, size_t length, sh_package_references *references)
{
    sh_decl_registry_source source;
    embed_preparation preparation = {0};
    uintptr_t reflection;
    int ok;
    if (!references) return 0;
    if (!json) return !length;
    if (!sh_json_validate(json, length, 128, NULL)) return 0;
    if (!sh_decl_server_registry_source(&source) ||
        !(reflection = (uintptr_t)sh_typeinfo_get_reflect())) { references->incomplete = 1; return 1; }
    preparation.references = references;
    preparation.native = sh_resource_graph_prepare_open(source, reflection);
    if (!preparation.native) { references->incomplete = 1; return 1; }
    ok = sh_package_map_states(json, length, embed_prepare_state, &preparation) &&
         sh_package_map_references(json, length, embed_prepare_reference, preparation.native);
    if (ok) {
        size_t i;
        for (i = 0; i < references->count; i++)
            embed_prepare_reference(preparation.native, references->items[i].type, references->items[i].name);
    }
    sh_resource_graph_prepare_close(preparation.native);
    if (!ok) sh_package_references_free(references);
    return ok;
}

size_t sh_mpkg_used_packages(const char *json, size_t len, const char *data_root,
                             sh_mpkg_used **out)
{
    const sh_package_compilation *compiled;
    sh_package_owners owners = {0};
    int dependencies_complete = 0;
    sh_package_references references = {0};
    size_t count = 0, i, capacity;
    sh_mpkg_used *selected = NULL;
    if (!out) return SIZE_MAX;
    free(*out); *out = NULL;
    (void)data_root;
    /* Source probes may enter the engine. Finish this pass before acquiring
     * the immutable compiler snapshot used for package ownership. */
    if (!sh_mpkg_prepare_map(json, len, &references)) return SIZE_MAX;
    compiled = sh_package_runtime_acquire();
    if (!compiled || !sh_package_map_owners(compiled, sh_package_runtime_catalog(), json, len,
                                           &references, &owners, &dependencies_complete)) goto bad;
    capacity = sh_package_owners_count(&owners);
    if (capacity > SIZE_MAX / sizeof(*selected) ||
        (capacity && !(selected = (sh_mpkg_used *)calloc(capacity, sizeof(*selected))))) goto bad;
    for (i = 0; i < compiled->sources->package_count; i++) if (sh_package_owners_contains(&owners, i)) {
        const sh_package_sources *sources = compiled->sources;
        size_t j;
        if (count == capacity) goto bad;
        for (j = 0; j < sources->component_count; j++) if (sources->components[j].owner == i && !sources->components[j].relative[0]) break;
        if (j == sources->component_count || strcpy_s(selected[count].id, sizeof(selected[count].id), sources->components[j].descriptor.id) ||
            strcpy_s(selected[count].root, sizeof(selected[count].root), sources->packages[i].root)) goto bad;
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
    sh_package_runtime_release(); sh_package_references_free(&references);
    free(selected); sh_package_owners_free(&owners); return SIZE_MAX;
}
