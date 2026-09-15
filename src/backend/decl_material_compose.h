#ifndef SH_DECL_MATERIAL_COMPOSE_H
#define SH_DECL_MATERIAL_COMPOSE_H
#include "decl_compose.h"
#include "decl_material.h"

typedef struct sh_decl_material_composition_schema {
    void *context;
    /* Source-bound lookup. For renderparm, binding identifies all schema/default
     * bytes relevant to its interpretation; it is copied before return to the
     * parser. Other resource kinds may leave it empty. No native loads. */
    int (*resolve)(void *context, sh_decl_composition_role role, size_t index,
        const char *family, sh_decl_source name, int *kind, sh_decl_source *binding);
} sh_decl_material_composition_schema;

/* Compose ordered material writes using original-relative field changes
 * and authored ordering. Full-write literal vectors compose by component;
 * masked or expression writes remain atomic. Reparse under the final source
 * schema and verify every retained prior-write/default binding. Repeated
 * destinations retain ordinal occurrences; changing multiplicity requires an
 * unchanged retained prefix. Unimplemented grammar refuses. Source ownership
 * and complete resource dependency retention remain the caller's obligation. */
char *sh_decl_material_compose(sh_decl_source baseline, const sh_decl_source *sources,
    size_t count, const sh_decl_material_composition_schema *schema,
    size_t *length, char *error, size_t capacity, sh_decl_conflict *conflict);
#endif
