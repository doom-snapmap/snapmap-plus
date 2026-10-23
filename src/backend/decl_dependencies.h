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
/* A by-name declaration reference: a reflected idStr subclass, stored by the
 * shared idStr reader, whose consumers later find the name in one manager with
 * makeDefault 0 (idEntityDefRef: entityDef FindByName, VK 0x1417b36f0 / GL
 * 0x1417a61b0). Its element {class, SH_DECL_BY_NAME_DECL_OPS} binds like the
 * class's typed pointer, but an empty value names nothing and a name with no
 * source stays a name-only identity, as the lookup returns null for it. */
#define SH_DECL_BY_NAME_DECL_OPS "@by-name-decl"
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
    int plain;                  /* IGNORE read by the generic scalar reader (a reflected primitive or enum
                                 * with no registered reader): reading it only assigns the value */
    int assigns;                /* sh_decl_assign_form: the token form this field's registered reader
                                 * assigns completely from that token alone (0 none) */
    int effects;                /* sh_decl_read_effects of its registered reader in every other form */
} sh_decl_value_shape;

/* Token forms a registered reader assigns totally: the field's value then
 * depends only on the token, never on the earlier value, and the read has no
 * other effect, so a later assignment hides an earlier one completely. */
typedef enum sh_decl_assign_form {
    SH_DECL_ASSIGN_NONE,
    SH_DECL_ASSIGN_QUOTED,      /* one quoted string token (idStr's reader and readers built on it) */
    SH_DECL_ASSIGN_NUMBER,      /* one decimal number, read as a float */
    SH_DECL_ASSIGN_INTEGER,     /* one decimal integer */
    SH_DECL_ASSIGN_TOKEN,       /* the tokens up to ';', unquoted and looked up in a static registry */
    SH_DECL_ASSIGN_NULL,        /* the unquoted NULL token, which the generic pointer dispatch stores */
    SH_DECL_ASSIGN_BOOLEAN      /* the name token true or false, case-sensitive */
} sh_decl_assign_form;

/* What a registered reader's body can change besides an assignment form.
 * FIELD readers change only their own field, from their tokens, its earlier
 * value, the parse's fixed arguments and static tables. GLOBAL readers also
 * look up, create, load, intern or allocate outside the field, or add the
 * per-call pending registrations SetEntityEditState flushes; they keep their
 * effects confined to the field otherwise. NONE values are skipped by the
 * native dispatch without any effect. Readers whose bodies are not modelled
 * stay UNMODELLED, which is unfinished coverage, not a native limit. */
typedef enum sh_decl_read_effects {
    SH_DECL_EFFECTS_UNMODELLED,
    SH_DECL_EFFECTS_FIELD,
    SH_DECL_EFFECTS_GLOBAL,
    SH_DECL_EFFECTS_NONE
} sh_decl_read_effects;

/* A verified reference location, borrowed for the duration of the walk. A
 * declaration supplies node; JSON supplies its exact original value span.
 * Observers must queue changes and apply them only after a successful walk.
 * Never mutate a node or the source buffer from a traversal callback. */
typedef struct sh_decl_reference_site {
    const sh_decl_node *node;
    const char *json;
    size_t json_length;
    /* The reflected class whose member holds the reference and that member's
     * key; a collection element reports its collection's owner and key. NULL
     * where the walk has no reflected owner (headers, event arguments, JSON). */
    const char *owner_type, *member;
    /* Event-declaration arguments only. Borrowed for this callback: selected
     * literal event name and counted argument context. signature is indexed
     * by argument number: native format letters, '0' for an explicit null,
     * '?' for an omitted slot; NULL if a supplied reader is unverified.
     * Its length ends at the highest supplied index, not at args.num. */
    const char *event_name;
    size_t event_name_length, event_argument_index, event_argument_count;
    const char *event_argument_signature;
} sh_decl_reference_site;

/* Where the struct reader stores a member: the byte offset from the object it
 * reads and the bytes stored there. A bitfield setter's verified store also
 * names the bits it assigns within those bytes (first bit, count). */
typedef struct sh_decl_member_storage { long long offset, extent; int bit_first, bit_count, assigns; } sh_decl_member_storage;

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
    /* Optional storage authority for a member field() resolves, by the same
     * first match through the class chain: 1 with its offset and the bytes its
     * type reader stores there inline; 2 written through a bitfield setter,
     * with a nonzero extent only when the setter's code is a verified masked
     * assignment of those bits, and assigns (sh_decl_assign_form) the token
     * form the bitfield reader parses into it without error or other
     * effect; 0 not a member; -1 unknown. Disjoint storage
     * is proved only from extents given here, never from names or declared types. */
    int (*storage)(void *context, sh_decl_value_type object_type, const char *key,
        sh_decl_member_storage *storage);
    /* Optional: 1 when an empty quoted literal of this type is a declaration
     * lookup, which the lookup (VK 0x1417b36f0 / GL 0x1417a61b0) renames to
     * _emptyName before its ordinary search. The walk then reports a reference
     * to _emptyName at the literal's site. The unquoted NULL token stores NULL
     * and is never reported. Without this callback an empty literal names
     * nothing; an empty inherit header names nothing either way, since its
     * parent lookup passes makeDefault 0. */
    int (*empty_lookup)(void *context, sh_decl_value_type type);
    /* Optional, event calls (idEventCallInfo): once per call with its literal
     * eventDef name (NULL when absent or not a literal), index SIZE_MAX and key
     * NULL; then once per argument whose reader key stores a value instead of
     * resolving a resource (every key but decl and md6Anim), with its index,
     * that key and the value's site. The handler a name selects is native
     * evidence, never inferred here. Return 0 to abort the walk. */
    int (*event_value)(void *context, const char *path, const char *event, size_t event_length,
        size_t index, const char *key, const sh_decl_reference_site *site);
    /* Optional primitive-value location, after value() accepts a text value.
     * JSON invokes this directly, with its original span and no node. The
     * reflected object/member provenance is structural, not a statement that
     * the value is an asset or runtime name. A collection element retains its
     * owning field; a nested object's members name that object's type. */
    int (*value_site)(void *context, const char *path, sh_decl_value_type type,
        const sh_decl_reference_site *site);
} sh_decl_dependency_schema;
/* The format letter selected by a native event argument reader; 0 means its
 * reader is not established. Named enum readers require schema evidence. */
char sh_decl_event_argument_format(const sh_decl_dependency_schema *schema, const char *key);
/* Native AddEvent limits event storage to eight argument slots. args.num is
 * a signed int record count, independent of the supplied slots' indices. A
 * nonpositive count consumes no argument records; an absent count is not
 * inferred from the fields. Decimal forms outside the native int stay gaps. */
enum { SH_DECL_EVENT_MAX_ARGUMENTS = 8 };
int sh_decl_event_argument_count(const char *text, size_t length, size_t *count);

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
/* The same over the statements native reads: the positional inherit header,
 * then the state statement sh_decl_entity_state_statement selects for state
 * (NULL text-local, "" first stateful layer, a name in update mode with every
 * later statement of that name). Inert statements are not inspected. */
int sh_decl_entity_state_dependencies(const sh_decl_node *definition, sh_decl_value_type entity_type,
    const char *state, const sh_decl_dependency_schema *schema, sh_decl_dependency_result *result);
/* Where the native state statement is undecided product coverage
 * (sh_decl_entity_merges_classified): the positional inherit header and every
 * block statement, so no statement native may read stays unbound. A coverage
 * superset reported with a gap by its caller, never a state selection. */
int sh_decl_entity_every_state_dependencies(const sh_decl_node *definition, sh_decl_value_type entity_type,
    const sh_decl_dependency_schema *schema, sh_decl_dependency_result *result);
/* Only the positional inherit header, a typed entityDef reference Parse looks
 * up by name. For a definition whose class has no reflected schema: its state
 * stays opaque and unbound (its caller reports that coverage), while its known
 * parent still binds to the parent actually served. */
int sh_decl_entity_header_dependencies(const sh_decl_node *definition,
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
