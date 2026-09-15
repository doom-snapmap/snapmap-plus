/* Fill deferred dependency records from already prepared native state. */
#ifndef SH_RESOURCE_GRAPH_PREPARED_H
#define SH_RESOURCE_GRAPH_PREPARED_H
#include "decl_native_registry.h"
#include "decl_dependencies.h"
typedef struct sh_resource_graph_preparation sh_resource_graph_preparation;
/* The caller binds the current registry, reflection and read-only source
 * probe at an engine main-thread boundary. Do not retain across mutations. */
sh_resource_graph_preparation *sh_resource_graph_prepare_open(sh_decl_registry_source source,
    uintptr_t reflection);
/* Returns 1 for a complete static walk, 0 for partial/unavailable or skipped
 * state. Known edges survive unsupported siblings.
 * It never constructs an entity, reparses a decl or replaces a complete state. */
int sh_resource_graph_prepare_entity(sh_resource_graph_preparation *preparation,
    const char *name, sh_decl_dependency_result *result);
/* Resolve inline entity edit JSON through reflected native field readers.
 * An absent class uses the inherited declaration's prepared class. Reports
 * references to the caller only; it never adds instance edits to asset graphs.
 * Partial coverage returns0 without aborted; callbacks can retain known roots. */
int sh_resource_graph_prepare_inline(sh_resource_graph_preparation *preparation,
    const char *class_name, const char *inherit, const char *json, size_t length,
    int (*visitor)(void *context, const char *type, const char *name), void *context,
    sh_decl_dependency_result *result);
void sh_resource_graph_prepare_close(sh_resource_graph_preparation *preparation);
#endif
