/* Typed dependencies from declaration state, without constructing game objects. */
#ifndef SH_DECL_DEPENDENCIES_H
#define SH_DECL_DEPENDENCIES_H

#include "decl_tree.h"

typedef struct sh_decl_value_type { const char *name, *ops; } sh_decl_value_type;

typedef enum sh_decl_value_kind {
    SH_DECL_VALUE_UNKNOWN,
    SH_DECL_VALUE_IGNORE,       /* verified to contain no resource references */
    SH_DECL_VALUE_OBJECT,       /* normal reflected members, including bases */
    SH_DECL_VALUE_REFERENCE,    /* a verified native resource-reader contract */
    SH_DECL_VALUE_COLLECTION,   /* a verified native collection-reader contract */
    SH_DECL_VALUE_POLYMORPHIC   /* native className/object wrapper; element is its base class */
} sh_decl_value_kind;

typedef struct sh_decl_value_shape {
    sh_decl_value_kind kind;
    sh_decl_value_type element;
    const char *item_key;       /* e.g. item; NULL uses this field's unindexed name */
    const char *count_key;      /* e.g. num; NULL for a fixed array */
    size_t count;
    int count_known;
} sh_decl_value_shape;

/* These adapters belong to the compiler, never package.json. Type strings and
 * shape strings returned by an adapter must remain valid throughout the walk.
 * field() resolves inherited fields: 1 found, 0 proven obsolete/ignored by the
 * native reader, -1 unknown. A custom property without an adapter is unknown.
 * describe() may return UNKNOWN; never infer references from arbitrary strings
 * or unwrap an unsupported template just because its name resembles a list. */
typedef struct sh_decl_dependency_schema {
    void *context;
    int (*describe)(void *context, sh_decl_value_type type, sh_decl_value_shape *shape);
    int (*field)(void *context, sh_decl_value_type object_type, const char *key,
                 sh_decl_value_type *field_type);
    int (*reference)(void *context, const char *path, sh_decl_value_type type,
                     const char *name, size_t name_length);
    void (*gap)(void *context, const char *path, sh_decl_value_type type, const char *reason);
    /* Resolve a selected native object class without constructing it. Require
     * reflected compatibility with base_type; return 1 with a borrowed type,
     * otherwise unavailable. This is a compiler adapter, never author policy. */
    int (*dynamic_type)(void *context, sh_decl_value_type base_type,
        const char *class_name, size_t length, sh_decl_value_type *selected_type);
} sh_decl_dependency_schema;

typedef struct sh_decl_dependency_result {
    size_t visited, references, gaps;
    int aborted;
} sh_decl_dependency_result;

/* Visit known edges even when siblings have unsupported state. Return 1 only
 * with complete coverage and successful callbacks. No engine code is called.
 * The caller supplies a resolved entity class and the complete effective state
 * for inherited-field replacement semantics. An own-state tree can also be
 * inspected, but its result alone does not establish an inherited closure. */
int sh_decl_state_dependencies(const sh_decl_node *state, sh_decl_value_type type,
    const sh_decl_dependency_schema *schema, sh_decl_dependency_result *result);

/* Inspect a full entityDef: emit its typed inherit edge and inspect only edit.
 * The top-level editorVars block and other declaration metadata are not state.
 * This emits the declaration's explicit contributions, not a merged parent. */
int sh_decl_entity_dependencies(const sh_decl_node *definition, sh_decl_value_type entity_type,
    const sh_decl_dependency_schema *schema, sh_decl_dependency_result *result);

/* Inspect an inline map edit object using the same native field schema.
 * JSON strings become references only where the native reader defines a
 * resource pointer. Literal escapes are decoded, null clears a pointer, and
 * known collection readers inspect JSON arrays. Unsupported shapes are gaps.
 * Full JSON validation precedes callbacks; malformed input aborts the walk. */
int sh_decl_json_state_dependencies(const char *json, size_t length, sh_decl_value_type type,
    const sh_decl_dependency_schema *schema, sh_decl_dependency_result *result);

#endif
