/* Reflected object dependencies inside the native graph envelope. */
#ifndef SH_DECL_GRAPH_DEPENDENCIES_H
#define SH_DECL_GRAPH_DEPENDENCIES_H
#include "decl_graph.h"
#include "decl_native_schema.h"

/* The canonical family binds the graph root; each polymorphic record must
 * derive from its native subgraph/node/link class. Inspect every occurrence,
 * keeping known references when other object readers are unsupported. Root
 * object state uses the native graph reader's generic-field dispatch contract.
 * Returns complete typed-reference coverage only, not late gameplay closure or
 * graph validity. No resources are loaded and no native readers are called. */
int sh_decl_graph_dependencies(const sh_decl_graph *graph, const char *declaration_type,
    sh_decl_native_schema *schema, sh_decl_dependency_result *result);

#endif
