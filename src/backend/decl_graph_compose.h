/* Composition of the registered native graph envelope. */
#ifndef SH_DECL_GRAPH_COMPOSE_H
#define SH_DECL_GRAPH_COMPOSE_H
#include "decl_compose.h"

/* The caller must establish graph-reader registration and root class ancestry.
 * Inner object state uses native reflection, including polymorphic readers.
 * Record names are scoped to their collection; repeated names retain ordinal
 * slots. A change of duplicate multiplicity requires unchanged common slots.
 * Endpoints carry internal occurrence references through the merge, then must
 * resolve to those same occurrences in the native output. No identifiers or
 * compiler metadata enter authored files or the served declaration.
 * Unsupported/ambiguous edits return NULL. Inputs remain untouched. */
char *sh_decl_graph_compose(sh_decl_source baseline,
    const sh_decl_source *sources, size_t count,
    const sh_decl_dependency_schema *types, sh_decl_value_type root_type,
    size_t *length, char *error, size_t error_capacity, sh_decl_conflict *conflict);
#endif
