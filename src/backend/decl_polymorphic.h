/* Shared interpretation of verified native className/object wrappers. */
#ifndef SH_DECL_POLYMORPHIC_H
#define SH_DECL_POLYMORPHIC_H
#include "decl_dependencies.h"

typedef struct sh_decl_polymorphic_state {
    sh_decl_value_type type;
    const sh_decl_node *object;
} sh_decl_polymorphic_state;

/* Accept an explicit class with optional object state, or the native scalar
 * class shorthand. Everything is borrowed. NULL forms and an empty class name
 * select no class: their result is the destination's prior state (see
 * sh_decl_polymorphic_unchanged) or no class at all, so they stay unavailable
 * to a caller that needs one.
 * Unsupported forms must stay atomic for composition and explicit dependency
 * gaps; never reinterpret them as ordinary field objects. Returns 1 resolved,
 * 0 unsupported form, -1 an explicit class could not be resolved compatibly. */
int sh_decl_polymorphic_read(const sh_decl_node *node, sh_decl_value_type base_type,
    const sh_decl_dependency_schema *schema, sh_decl_polymorphic_state *state,
    const char **reason);

/* The registered idTypeInfoObjectPtr reader (Vulkan 0x1409D0660, OpenGL
 * 0x1409CFE20) returns without touching the pointer or its object for NULL
 * (quoted or not; it warns "NULL entry for idTypeInfoObjectPtr"), for an
 * empty block, and for a block whose className is NULL (the rest of the block
 * is skipped). Whatever the destination held stays, so such a value selects and
 * references nothing of its own. Only a shape with null_keeps has this reader;
 * other polymorphic readers (graph objects) must not use this. */
int sh_decl_polymorphic_unchanged(const sh_decl_node *node);

/* idSSObject has an object selector followed by fields in the same block.
 * Empty blocks and native NULL clear the object. The returned shallow block
 * borrows the original field nodes so reference-site identities stay exact. */
int sh_decl_script_read(const sh_decl_node *node, sh_decl_value_type base_type,
    const sh_decl_dependency_schema *schema, sh_decl_value_type *selected,
    sh_decl_node *fields, const char **reason);
#endif
