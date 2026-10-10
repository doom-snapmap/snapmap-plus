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
    SH_DECL_VALUE_POLYMORPHIC,  /* native className/object wrapper; element is its base class */
    SH_DECL_VALUE_SCRIPT,      /* object selector followed by script-module fields */
    SH_DECL_VALUE_EVENT_CALL   /* idEventCallInfo: event name, then counted typed arguments */
} sh_decl_value_kind;

/* An event-call decl argument names its declaration type, which the native
 * argument reader looks up among the declaration managers before finding the
 * name with makeDefault. Walkers describe {type, SH_DECL_EVENT_DECL_OPS}; a
 * schema that can prove the manager answers REFERENCE with element set to the
 * typed declaration pointer it binds. Any other answer leaves the argument a
 * gap. This internal spelling never becomes a resource or authored name. */
#define SH_DECL_EVENT_DECL_OPS "@event-decl"
/* The element of a proven event decl argument, {class, SH_DECL_EVENT_DECL_USE_OPS},
 * is read by the class's declaration pointer reader like a reflected field,
 * but keeps the use distinct: a name with no source is the engine's default
 * for this use wherever it is authored, including a map's own eventCall. */
#define SH_DECL_EVENT_DECL_USE_OPS "@event-decl-use"
/* An event-call md6Anim argument names an animation, which the native
 * argument reader finds in the animation resource list and otherwise loads
 * with makeDefault. Walkers describe {"idMD6Anim", SH_DECL_EVENT_ANIM_OPS};
 * a schema that proves that list answers REFERENCE with element
 * {"idMD6Anim", SH_DECL_EVENT_ANIM_OPS}, read by the typed resource pointer it
 * binds, so consumers keep the use's makeDefault distinct from a reflected
 * idMD6Anim field. Any other answer leaves the argument a gap. */
#define SH_DECL_EVENT_ANIM_OPS "@event-md6anim"
#define SH_DECL_EVENT_ANIM_TYPE "idMD6Anim"

/* The native event-call writer may annotate a member with its type, as in
 * "
eventHandle_t eventDef" or "
num"; return the member's bare name. */
const char *sh_decl_event_member(const char *key);
/* An event argument's reader key: 0 reads no resource, 1 decl, 2 md6Anim. */
int sh_decl_event_argument_kind(const char *key);


typedef struct sh_decl_value_shape {
    sh_decl_value_kind kind;
    sh_decl_value_type element;
    const char *item_key;       /* e.g. item; NULL uses this field's unindexed name */
    const char *count_key;      /* e.g. num; NULL for a fixed array */
    size_t count;
    int count_known;
    int null_keeps;             /* polymorphic: the reader skips NULL forms without touching the destination */
} sh_decl_value_shape;

/* A verified reference location, borrowed for the duration of the walk. A
 * declaration supplies node; JSON supplies its exact original value span.
 * Observers must queue changes and apply them only after a successful walk.
 * Never mutate a node or the source buffer from a traversal callback. */
typedef struct sh_decl_reference_site {
    const sh_decl_node *node;
    const char *json;
    size_t json_length;
} sh_decl_reference_site;

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
    int (*reference_site)(void *context, const char *path, sh_decl_value_type type,
        const char *name, size_t name_length, const sh_decl_reference_site *site);
    /* Optional: every value whose type is verified to hold no resource
     * reference (primitives, strings, enums, registered value readers), with
     * its reflected type. Return 0 to abort the walk. */
    int (*value)(void *context, const char *path, sh_decl_value_type type, const sh_decl_node *node);
} sh_decl_dependency_schema;

typedef struct sh_decl_dependency_result {
    size_t visited, references, gaps;
    int aborted;
} sh_decl_dependency_result;

/* Native pointer-reader literal. An unquoted NULL clears the pointer; a
 * quoted "NULL" remains a name. Empty names have length zero. */
int sh_decl_reference_literal(const sh_decl_node *node, const char **name, size_t *length);

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

/* The reflected declaration envelope uses the same ordered edit/inherit sites
 * with a family-specific parent pointer. Every occurrence is visited; this is
 * binding, not a choice of which assignment the native reader will retain. */
int sh_decl_envelope_dependencies(const sh_decl_node *definition, sh_decl_value_type state_type,
    sh_decl_value_type parent_type, const sh_decl_dependency_schema *schema,
    sh_decl_dependency_result *result);

/* Inspect an inline map edit object using the same native field schema.
 * JSON strings become references only where the native reader defines a
 * resource pointer. Literal escapes are decoded, null clears a pointer, and
 * known collection readers inspect JSON arrays. Unsupported shapes are gaps.
 * Full JSON validation precedes callbacks; malformed input aborts the walk. */
int sh_decl_json_state_dependencies(const char *json, size_t length, sh_decl_value_type type,
    const sh_decl_dependency_schema *schema, sh_decl_dependency_result *result);

#endif
