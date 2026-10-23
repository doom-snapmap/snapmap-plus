/* Typed projections from saved values to native runtime-name lookup keys. */
#ifndef SH_PACKAGE_NATIVE_NAMEFLOW_H
#define SH_PACKAGE_NATIVE_NAMEFLOW_H

#include "decl_dependencies.h"
#include <stdint.h>

typedef enum sh_native_nameflow_kind {
    SH_NATIVE_NAMEFLOW_NONE,
    SH_NATIVE_NAMEFLOW_SCRIPT_CALL,
    SH_NATIVE_NAMEFLOW_UNAVAILABLE,
    SH_NATIVE_NAMEFLOW_INACTIVE
} sh_native_nameflow_kind;

/* Classify a primitive member at its structural typed site. The currently
 * recovered members are idTrigger.call and idTarget_Count.call. Their native
 * CallScriptFunction consumer selects the first-dot prefix as a FindEntity
 * key. Reflected type and storage must agree with the actual consumer reads;
 * matching a member's text alone establishes nothing. This adds consumers,
 * never exempts unclassified values or establishes complete name-flow coverage.
 * derives has the ordinary native class relation: 1 derives, 0 unrelated,
 * -1 unavailable, and must validate the named class even against itself. */
sh_native_nameflow_kind sh_native_nameflow_member(const sh_decl_dependency_schema *schema,
    void *context, int (*derives)(void *context, const char *class_name, const char *base_name),
    sh_decl_value_type type, const sh_decl_reference_site *site);

/* Borrow an effective native state (the same prepared inheritance/overlay
 * view the compiler evaluates), rooted at its edit object. The enum callback
 * resolves the actual selected enum reader, never a spelling convention.
 * Missing sparse/default state is unknown, not an inactive selector. */
typedef struct sh_native_nameflow_scope {
    const sh_decl_node *state;
    sh_decl_value_type type;
    void *context;
    int (*enum_value)(void *context,sh_decl_value_type type,
        const sh_decl_node *token,int32_t *value);
} sh_native_nameflow_scope;
/* The five paired direct CallScriptFunction consumer families: trigger and
 * count call fields, narrative script actions/conditions, spline Script.
 * Returns the borrowed effective string for SCRIPT_CALL. INACTIVE proves
 * this particular consumer performs no lookup; it does not rewrite data or
 * certify other consumers. Unsupported storage/selector/defaults remain
 * UNAVAILABLE. path is the typed walk's exact structural path. */
sh_native_nameflow_kind sh_native_nameflow_scoped_member(const sh_decl_dependency_schema *schema,
    void *context,int (*derives)(void *,const char *,const char *),sh_decl_value_type type,
    const sh_decl_reference_site *site,const char *path,const sh_native_nameflow_scope *scope,
    const char **value,size_t *length);

/* Borrow the actual C-string lookup key, without modifying the source value.
 * 1 projected key, 0 no entity lookup (empty or no dot), -1 unsupported input.
 * The first dot alone separates entity from function; a dollar prefix has no
 * special meaning here. Empty keys still reach FindEntity. Embedded NUL is
 * left unknown rather than pretending the serialized suffix is consumed. */
int sh_native_nameflow_key(sh_native_nameflow_kind kind, const char *value, size_t length,
    const char **key, size_t *key_length);

#endif
