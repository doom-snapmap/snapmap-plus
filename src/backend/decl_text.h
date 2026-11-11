/* decl_text.h -- bounded structural validation shared by decl file consumers. */
#ifndef BACKEND_DECL_TEXT_H
#define BACKEND_DECL_TEXT_H

#include <stddef.h>

/* One admitted decl body participating in deterministic dependency ordering.
 * `value` is opaque caller state and moves with the item. */
typedef struct sh_decl_reference_item {
    const char *name;
    const unsigned char *text;
    size_t text_length;
    void *value;
    const char *type;
} sh_decl_reference_item;

/* Return 1 when `text` is non-empty, contains no embedded NUL, and has balanced
 * braces, quotes, and comments. This is deliberately structural only; DOOM's
 * own decl parser remains the semantic authority. */
int sh_decl_text_well_formed(const unsigned char *text, size_t length);

/* Return 1 when a snapEditorEntityDef body contains a real top-level
 * `inherit = ...` assignment or a direct `edit.entityDef = ...` assignment.
 * The check is lexical only: comments and quoted values are ignored, key
 * matching is ASCII case-insensitive, and nested fields below edit are not
 * mistaken for the direct entityDef field. This is the conservative gate for
 * deciding whether a newly scanned sedef is worth a native make-default call.
 */
int sh_decl_text_sedef_has_materializable_source(const unsigned char *text,
                                                 size_t length);

/* A typed dependency emitted from the small subset of decl fields that the
 * editor palette materializer must resolve. `name` is a view into `text` and
 * is not NUL-terminated. The callback returns zero to refuse the traversal. */
typedef int (*sh_decl_text_dependency_fn)(const char *type,
                                          const unsigned char *name,
                                          size_t name_length,
                                          void *context);

/* Emit the explicit dependency edges used by a new snapEditorEntityDef:
 * same-type top-level inherit, direct edit.entityDef, and
 * edit.buildGameRefEntityDefs item values. */
int sh_decl_text_collect_sedef_dependencies(const unsigned char *text,
                                            size_t length,
                                            sh_decl_text_dependency_fn callback,
                                            void *context);

/* Emit the top-level inherit edge of an entityDef. */
int sh_decl_text_collect_entitydef_dependencies(const unsigned char *text,
                                                size_t length,
                                                sh_decl_text_dependency_fn callback,
                                                void *context);

/* Reorder an already-admitted set so uniquely resolved logical names quoted by
 * one body are registered before the body that references them. Material
 * expressions also resolve unquoted name[index] references to table decls;
 * callers provide `type` to enable these typed edges. Comments and
 * escaped strings are ignored; ambiguous names create no edge. Strongly
 * connected components are condensed and selected by logical-name order, while
 * members of a component are also name-ordered unless explicit inherit edges
 * require a parent-first order. A true explicit-inherit cycle refuses the
 * snapshot (return 0), as does bounded scratch allocation failure. */
int sh_decl_text_order_by_references(sh_decl_reference_item *items, size_t count,
                                     size_t *out_edge_count,
                                     size_t *out_cycle_count);

/* A placed entity's own decl text from its prepared text (`prepared`, the
 * inherit chain merged root first with its own members) and its inherit's
 * prepared text (`inherited`; may be empty). Every leaf whose key path and
 * value equal the inherit's is dropped, as the engine drops them when it
 * reads a saved entity, and so is a block left empty; the first statement is
 * always written. The result is tab-indented `key = value;` lines. Returns 0,
 * with `out` empty, for a malformed or truncated text or a full `out`. */
int sh_decl_text_own_state(const unsigned char *prepared, size_t prepared_length,
                           const unsigned char *inherited, size_t inherited_length,
                           char *out, size_t capacity, size_t *out_length);

#endif /* BACKEND_DECL_TEXT_H */
