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
    SH_BINDING_ANIMATION_WEB,
    SH_BINDING_ENVIRONMENT,
    SH_BINDING_CLOTH
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

/* A render program declaration: one braced body of outer commands, an optional
 * leading `inherit <program>`, then named blocks (permutations, parms, state,
 * flags, inout, computelayout, renderoutput, buffer, compileroptions,
 * wavelimits, hlsl_header, hlsl_vp_header, hlsl_fp_header, hlsl_cp_header,
 * hlsl_vp, hlsl_fp, hlsl_cp). Each block is opaque balanced text except parms,
 * which reads with the material grammar (its render parameters and values,
 * through schema). Sites: the inherited program (family renderprog) and the
 * parms block's references, at offsets in source; an assembled reference keeps
 * offset SIZE_MAX. Shader preprocessing is not expanded and HLSL, state, flags
 * or defines are not interpreted. 1 read, 0 refused (error set). */
int sh_decl_render_program_sites(sh_decl_source source, const sh_decl_material_schema *schema,
    sh_resource_binding_site visit, void *context, char *error, size_t capacity);

/* A skin declaration, as idDeclSkins::Parse reads it (Vulkan 0x1417bb580,
 * OpenGL 0x1417ae0a0): one braced body of channels, each a name and a braced
 * list read two tokens at a time, every token the name of a material the
 * material manager finds with makeDefault (an unknown material is the default
 * material, not a parse error). An empty body or channel is a skin with no
 * references. Sites: every material name, family material, at its offset in
 * source (quotes excluded; an empty quoted name has no site). Odd lists,
 * punctuation in a name position and trailing text refuse. 1 read, 0 refused
 * (error set). */
int sh_decl_skin_sites(sh_decl_source source, sh_resource_binding_site visit, void *context,
    char *error, size_t capacity);
#endif
