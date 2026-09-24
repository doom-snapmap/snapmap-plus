/* Source binding for non-assignment declaration grammars. */
#ifndef SH_DECL_NATIVE_REBIND_H
#define SH_DECL_NATIVE_REBIND_H
#include "decl_material.h"
#include "resource_rebind.h"

typedef enum sh_decl_native_binding_format {
    SH_BINDING_MATERIAL,
    SH_BINDING_RENDERPARM,
    SH_BINDING_MD6_DEFINITION,
    SH_BINDING_TABLE,
    SH_BINDING_ARTICULATED_FIGURE,
    SH_BINDING_BREAKABLE,
    SH_BINDING_ANIMATION_WEB
} sh_decl_native_binding_format;

/* One family dispatch for discovery, relocation and reflected-string exclusion.
 * These grammars have no idStrId leaves. Returns zero for other native readers. */
int sh_decl_native_binding_family(const char *family, sh_decl_native_binding_format *format);

/* Typed sites use exact source name bytes, excluding quotes and operators.
 * SIZE_MAX offset denotes a proven reference assembled by the native reader
 * from several tokens (StructuredBuffer types). It is a dependency, but not a
 * writable token. Sampler state identifiers are renderer state, not resources. */
int sh_decl_native_reference_sites(sh_decl_native_binding_format format, sh_decl_source source,
    const sh_decl_material_schema *schema, sh_resource_binding_site visitor, void *context,
    char *error, size_t capacity);
/* All-or-nothing native token edits. Unselected text remains byte-identical.
 * A request to rename an assembled reference is refused, never silently lost.
 * Shader parameter names may be renderer ABI; the caller must establish that a
 * replacement is legal for the consuming program before requesting it here. */
char *sh_decl_native_rebind(sh_decl_native_binding_format format, sh_decl_source source,
    const sh_decl_material_schema *schema, sh_resource_binding_resolver resolver, void *context,
    size_t *output_length, size_t *changed, char *error, size_t capacity);
#endif
