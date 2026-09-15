/* Compose supported assignment-style declarations against a known baseline. */
#ifndef SH_DECL_COMPOSE_H
#define SH_DECL_COMPOSE_H

#include <stddef.h>

#include "decl_tree.h"
#include "decl_dependencies.h"

/* Compare parsed entity definitions while excluding the engine's top-level
 * editorVars authoring block. Return 1 only with a proven gameplay match;
 * unsupported syntax returns 0 and never silently drops a dependency. */
int sh_decl_entity_gameplay_equal(sh_decl_source baseline, sh_decl_source source);

/* These rules are supplied by verified engine-family adapters, never package
 * metadata. NULL item_key identifies a scalar collection; an object collection
 * names its stable direct child key. An empty key retains fixed positional
 * containers such as editor sheets; their count must remain unchanged.
 * A path may use [*] for a numeric index in a verified nested collection.
 * Semantic collections preserve compatible authored ordering constraints;
 * independent additions use identity order only to break unconstrained ties.
 * Index-referenced collections require a separate verified relocation contract. */
typedef struct sh_decl_collection_rule {
    const char *path;
    const char *item_key;
} sh_decl_collection_rule;

/* Native adapters derive extra ordering from merged entry references before
 * identity tie-breaking. Opposing authored relations remain conflicts. This
 * pure synchronous callback must not retain its borrowed nodes. Return -1 for
 * left-before-right, 1 for right-before-left, 0 unconstrained, or 2 incompatible.
 * Reversing arguments must reverse the relation. This is a compiler format
 * contract, never an author-supplied setting. */
typedef struct sh_decl_collection_order {
    void *context;
    int (*relation)(void *context, const char *path,
        const sh_decl_node *left, const sh_decl_node *right);
} sh_decl_collection_order;

/* Zero-based source indices for a directly identified conflict. SIZE_MAX
 * means no individual source (e.g. the baseline or a cycle involving several
 * contributors). The optional report is initialized on every call. */
typedef struct sh_decl_conflict {
    size_t first, second;
} sh_decl_conflict;

/* Compose only scalar root metadata from complete source declarations. This
 * resolves parent/class source views before state can be interpreted. The
 * returned text is malloc-owned and contains no edit block. Input state is
 * parsed for syntax but neither interpreted nor composed. */
char *sh_decl_compose_root_metadata(sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, size_t *out_length,
    char *error, size_t error_capacity, sh_decl_conflict *conflict);

typedef enum sh_decl_composition_role {
    SH_DECL_COMPOSITION_ORIGINAL,
    SH_DECL_COMPOSITION_CONTRIBUTION,
    SH_DECL_COMPOSITION_RESULT
} sh_decl_composition_role;

/* The dependency walker and composer share native field/reader metadata.
 * state_type describes the declaration's edit block when all inputs share a
 * type. The resolver supplies separate input and result types when declaration
 * headers or selected parents can change the class.
 * Fixed native arrays preserve sparse positions and extent. Dynamic lists still
 * need verified identities/order/index-reference contracts supplied by rules.
 * No callbacks may load resources or mutate the game during composition. */
typedef struct sh_decl_composition_schema {
    const sh_decl_dependency_schema *types;
    sh_decl_value_type state_type;
    void *context;
    /* Optional per-input resolution replaces the uniform state_type above.
     * Original/contribution receive their complete tree; result receives the
     * merged scalar root metadata before state is merged. The index identifies
     * a contribution only. Returned type strings live through composition.
     * Return 1 resolved, 0 only for an input with no edit state, -1 unavailable.
     * No native loading or mutation. Parent source views belong to the caller. */
    int (*resolve)(void *context, const sh_decl_node *definition,
        sh_decl_composition_role role, size_t index, sh_decl_value_type *type,
        char *error, size_t error_capacity);
} sh_decl_composition_schema;

/* Return a malloc-owned native declaration or NULL with a conflicting path or
 * syntax diagnostic. All sources are merged together, including new nested
 * objects, so intermediate results cannot invent author ordering constraints.
 * Every input remains untouched. A baseline is required;
 * callers must establish its provenance. Unsupported native grammars refuse
 * composition and can still be served unchanged as opaque resource files. */
char *sh_decl_compose(sh_decl_source baseline, const sh_decl_source *sources,
                       size_t count, const sh_decl_collection_rule *rules,
                       size_t rule_count, size_t *out_length,
                       char *error, size_t error_capacity, sh_decl_conflict *conflict);

char *sh_decl_compose_ordered(sh_decl_source baseline, const sh_decl_source *sources,
    size_t count, const sh_decl_collection_rule *rules, size_t rule_count,
    const sh_decl_collection_order *order, size_t *out_length,
    char *error, size_t error_capacity, sh_decl_conflict *conflict);

char *sh_decl_compose_typed(sh_decl_source baseline, const sh_decl_source *sources,
    size_t count, const sh_decl_collection_rule *rules, size_t rule_count,
    const sh_decl_composition_schema *schema, size_t *out_length,
    char *error, size_t error_capacity, sh_decl_conflict *conflict);

/* The compiler knows the native family from the engine path. Apply its output
 * grammar as well as the shared merge rules. In particular entityDef headers
 * must precede state in the native parser's order, including newly added ones.
 * A NULL schema retains the untyped merge path while runtime binding is built. */
char *sh_decl_compose_resource(const char *declaration_type,
    sh_decl_source baseline, const sh_decl_source *sources,
    size_t count, const sh_decl_collection_rule *rules, size_t rule_count,
    const sh_decl_composition_schema *schema, size_t *out_length,
    char *error, size_t error_capacity, sh_decl_conflict *conflict);

#endif
