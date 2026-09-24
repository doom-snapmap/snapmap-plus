/* Bind verified native resource references without editing author source. */
#ifndef SH_DECL_REBIND_H
#define SH_DECL_REBIND_H

#include "decl_dependencies.h"

/* Return zero to retain the name, one with a replacement, or negative on
 * failure. The replacement is borrowed only until this call returns. Types
 * and field paths come from the native reader, never a string heuristic. */
typedef int (*sh_decl_binding_resolver)(void *context, const char *path,
    sh_decl_value_type type, const char *name, size_t length, const char **replacement);
/* Same contract with the verified source location, borrowed during callback. */
typedef int (*sh_decl_site_binding_resolver)(void *context, const char *path,
    sh_decl_value_type type, const char *name, size_t length,
    const sh_decl_reference_site *site, const char **replacement);

typedef struct sh_decl_rebind_result {
    sh_decl_dependency_result dependencies;
    size_t changed;
} sh_decl_rebind_result;

/* The schema supplies type semantics and optional gap diagnostics. Its
 * reference callbacks are not invoked: resolver owns this traversal's edges.
 * Return malloc-owned output only after a fully covered, successful walk.
 * Failure returns NULL, leaves source intact, and reports no committed edits.
 * A no-op returns an exact source copy. JSON replacements preserve every byte
 * outside the verified reference values, including native non-UTF-8 strings. */
char *sh_decl_rebind_entity(sh_decl_source source, sh_decl_value_type entity_type,
    const sh_decl_dependency_schema *schema, sh_decl_binding_resolver resolver,
    void *context, size_t *output_length, sh_decl_rebind_result *result);
char *sh_decl_rebind_state(sh_decl_source source, sh_decl_value_type type,
    const sh_decl_dependency_schema *schema, sh_decl_binding_resolver resolver,
    void *context, size_t *output_length, sh_decl_rebind_result *result);
char *sh_decl_rebind_json(sh_decl_source source, sh_decl_value_type type,
    const sh_decl_dependency_schema *schema, sh_decl_binding_resolver resolver,
    void *context, size_t *output_length, sh_decl_rebind_result *result);

/* Custom reflected envelopes (including graphs) retain their own grammar and
 * enumerate typed state through the supplied observation view. The reader must
 * visit only nodes of syntax, return complete coverage, and avoid mutation.
 * Both the borrowed syntax and source remain unchanged, including on failure. */
typedef int (*sh_decl_binding_walk)(void *context, const sh_decl_dependency_schema *view,
    sh_decl_dependency_result *result);
char *sh_decl_rebind_parsed(sh_decl_source source, const sh_decl_node *syntax,
    const sh_decl_dependency_schema *schema, sh_decl_binding_walk reader, void *reader_context,
    sh_decl_binding_resolver resolver, void *context, size_t *output_length,
    sh_decl_rebind_result *result);
/* A custom JSON envelope can enumerate several typed states against one source
 * buffer. Reference sites must borrow that buffer. Repeated observation of one
 * site deduplicates only when it requests the same replacement. */
char *sh_decl_rebind_json_walk(sh_decl_source source,
    const sh_decl_dependency_schema *schema, sh_decl_binding_walk reader, void *reader_context,
    sh_decl_binding_resolver resolver, void *context, size_t *output_length,
    sh_decl_rebind_result *result);
char *sh_decl_rebind_json_walk_sites(sh_decl_source source,
    const sh_decl_dependency_schema *schema, sh_decl_binding_walk reader, void *reader_context,
    sh_decl_site_binding_resolver resolver, void *context, size_t *output_length,
    sh_decl_rebind_result *result);

#endif
