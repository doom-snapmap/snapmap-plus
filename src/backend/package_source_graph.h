/* Dependency inspection of a retained candidate's declaration bytes. */
#ifndef SH_PACKAGE_SOURCE_GRAPH_H
#define SH_PACKAGE_SOURCE_GRAPH_H

#include "package_usage.h"
#include "decl_native_registry.h"
#include "decl_native_schema.h"

typedef struct sh_package_source_graph_report {
    size_t declarations, references, gaps;
    char first_gap[1024];
} sh_package_source_graph_report;

/* Inspect candidate bytes without publishing them or changing the observed
 * native graph. The original reader must bypass live overrides. The caller
 * supplies a retained effective compilation (map over local) and stable native
 * metadata. Known paths survive unsupported readers; incomplete stays explicit.
 * This resolves explicit source references and supported expanded entity state,
 * including overridden parent fields and truncated inherited lists. Verified
 * sound-name conversion selects candidate bank event memberships, retaining
 * every matching locale path and each path's existing delivery ownership.
 * Installed/packed audio and streamed media still need their provider indexes.
 * Other custom grammars, unexpanded inheritance and late gameplay string consumers still
 * require adapters. It cannot alone certify engine closure.
 * Output starts zeroed and is replaced on success; failure clears it. */
int sh_package_source_graph(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const char *json, size_t length, sh_package_references *out,
    sh_package_source_graph_report *report, char *error, size_t capacity);

#endif
