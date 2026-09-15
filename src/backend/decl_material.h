/* Ordered material writes and their source-bound resource references. */
#ifndef SH_DECL_MATERIAL_H
#define SH_DECL_MATERIAL_H
#include "decl_native_lex.h"

typedef struct sh_decl_material_schema {
    void *context;
    /* 1 accepted, 0 absent, -1 unreadable. Renderparm requires a source-bound
     * kind (0..18); table requires source identity lookup to distinguish it from
     * a renderparm. Image/program/sampler names are already typed by the native
     * grammar: callers may collect them for later availability admission.
     * Acceptance alone is not a resource-availability proof. The callback must
     * not consult active loaded resources to select a parameter schema. */
    int (*resolve)(void *context, const char *family, sh_decl_source name, int *kind);
} sh_decl_material_schema;

typedef struct sh_decl_material_reference {
    const char *family;
    sh_decl_source name; /* Borrowed source or reader-owned name storage. */
    size_t write;
    int kind;
} sh_decl_material_reference;

typedef struct sh_decl_material_write {
    sh_decl_source name, value;
    int kind;
    unsigned mask;
    size_t line, first_reference, reference_count;
    /* A direct numeric constant has four positional values after native scalar
     * broadcast/short-vector zero filling. No arithmetic is evaluated here. */
    sh_decl_source components[4];
    int constant;
} sh_decl_material_write;

typedef struct sh_decl_material {
    sh_decl_tokens tokens;
    sh_decl_material_write *writes;
    size_t count;
    sh_decl_material_reference *references;
    size_t reference_count;
} sh_decl_material;

/* Read the native parameter block, retaining order and repeated destinations.
 * The source bytes remain borrowed. A typed custom adapter owns composition,
 * prior-write binding, schema compatibility and resource dependency retention.
 * Refuse unresolved/custom syntax instead of dividing values by newlines. */
int sh_decl_material_read(sh_decl_source source, const sh_decl_material_schema *schema,
    sh_decl_material *out, char *error, size_t capacity);
void sh_decl_material_free(sh_decl_material *material);
/* Project the native type prefix from a complete renderparm source envelope.
 * This does not validate its defaults, buffer layout or native consumer state.
 * Callers bind the complete source, not just this kind. */
int sh_decl_material_parameter_kind(sh_decl_source source, int *kind,
    char *error, size_t capacity);

typedef struct sh_decl_renderparm {
    sh_decl_tokens tokens;
    int kind;
    sh_decl_source value, edit_specifiers;
    sh_decl_material_reference *references;
    size_t reference_count;
    char *type_name; /* Owned concatenated StructuredBuffer type, when present. */
} sh_decl_renderparm;

/* Read supported native parameter defaults for source dependency inspection.
 * Vec defaults are literals; images have typed options; Program/Sampler "0"
 * is a null default; String defaults consume through the closing brace.
 * StructuredBuffer names can reference struct renderparms; raw struct bodies,
 * uniform-buffer sizes and image-store format enums are not file references.
 * Trailing edit specifiers are retained, not interpreted or validated.
 * Source bytes are borrowed through free. This
 * is not the grammar of a parameter value inside a material block. */
int sh_decl_renderparm_read(sh_decl_source source, const sh_decl_material_schema *schema,
    sh_decl_renderparm *out, char *error, size_t capacity);
void sh_decl_renderparm_free(sh_decl_renderparm *parameter);
#endif
