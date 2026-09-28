/* The native inline entity-state carrier: SnapMap saves an inline entityDef's
 * own members as JSON `state` and loads them back as declaration text.
 * Readers/writers (VK/GL): JSON->decl FUN_141a86d60 / FUN_141a79220,
 * decl->JSON FUN_141a89bc0 / FUN_141a7c080, float text FS_WriteFloatString
 * FUN_141a1a760 / FUN_141a0c9b0, token numbers idToken::NumberValue
 * FUN_141a0e290 / FUN_141a004c0, JSON numbers JsonCpp decodeNumber
 * FUN_141a5c350 / FUN_141a4e830 and valueToString FUN_141a85670 / FUN_141a77b30. */
#ifndef SH_DECL_NATIVE_CARRIER_H
#define SH_DECL_NATIVE_CARRIER_H

#include "decl_tree.h"

typedef enum sh_decl_carrier_limit {
    SH_DECL_CARRIER_EXACT = 0,
    /* The native carrier itself cannot represent the authored value (a mod
     * boundary, not missing product coverage). */
    SH_DECL_CARRIER_ENGINE,
    /* The carrier can hold a value, but its equivalence for this field's native
     * reader is not established by the product: a tracked product gap. */
    SH_DECL_CARRIER_GAP
} sh_decl_carrier_limit;

typedef struct sh_decl_carrier_refusal {
    sh_decl_carrier_limit limit;
    const char *reason; /* static text; NULL with EXACT means allocation failure */
    char path[256];     /* dotted member path of the refused value */
} sh_decl_carrier_refusal;

/* Per-leaf facts: the reflected type name ("float", "int", an enum or class
 * name; NULL when the field's native reader is unknown), or verbatim JSON for a
 * value that already came from native map JSON and is emitted unchanged. */
typedef struct sh_decl_carrier_leaf {
    const char *type;
    const char *json;
} sh_decl_carrier_leaf;
typedef void (*sh_decl_carrier_leaf_info)(void *context, const sh_decl_node *leaf,
    sh_decl_carrier_leaf *info);

/* One authored scalar spelling. Returns 1 with the malloc-owned JSON value
 * text that loads back to a value the field's native reader parses exactly
 * like the authored spelling, and survives native save/load unchanged; 0 when
 * not carriable (refusal filled); -1 on allocation failure. Only a decimal
 * spelling depends on field_type: SnapMap rewrites it through `%1.10f`, which
 * is exact only for float fields and only for values proven to round trip.
 * Every other accepted spelling loads back byte-identical, whatever its type. */
int sh_decl_carrier_scalar(const char *value, const char *field_type, char **json,
    sh_decl_carrier_refusal *refusal);

/* The children of members become the JSON `state` object, in authored order.
 * Blocks must use the carrier's own `key = {` form; reset markers, type
 * annotations and keys the writer would quote have no exact carrier spelling.
 * Returns malloc-owned JSON text, or NULL with refusal filled (allocation
 * failure leaves limit EXACT and reason NULL). */
char *sh_decl_carrier_state(const sh_decl_node *members, sh_decl_carrier_leaf_info info,
    void *context, size_t *length, sh_decl_carrier_refusal *refusal);

/* The declaration text SnapMap writes when it loads one JSON `state` value
 * (FUN_141a86d60 / FUN_141a79220): objects become `key = {` blocks, strings are
 * escaped and quoted, reals pass through FS_WriteFloatString "%f", integers
 * through asInt/asUInt, and arrays are refused as the native reader does.
 * Returns an owned compound node holding the members of a JSON object, or
 * NULL with error. verbatim(context, leaf, json, length) receives every leaf
 * node with its original JSON span so callers can re-emit it unchanged. */
sh_decl_node *sh_decl_carrier_tree(const char *json, size_t length,
    int (*verbatim)(void *context, const sh_decl_node *leaf, const char *json, size_t length),
    void *context, char *error, size_t capacity);

#endif
