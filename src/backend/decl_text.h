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
 * one body are registered before the body that references them. Comments and
 * escaped strings are ignored; ambiguous names create no edge. Strongly
 * connected components are condensed and selected by logical-name order, while
 * members of a component are also name-ordered unless explicit inherit edges
 * require a parent-first order. A true explicit-inherit cycle refuses the
 * snapshot (return 0), as does bounded scratch allocation failure. */
int sh_decl_text_order_by_references(sh_decl_reference_item *items, size_t count,
                                     size_t *out_edge_count,
                                     size_t *out_cycle_count);

#endif /* BACKEND_DECL_TEXT_H */
