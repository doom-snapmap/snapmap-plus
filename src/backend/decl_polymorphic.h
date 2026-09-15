/* Shared interpretation of verified native className/object wrappers. */
#ifndef SH_DECL_POLYMORPHIC_H
#define SH_DECL_POLYMORPHIC_H
#include "decl_dependencies.h"

typedef struct sh_decl_polymorphic_state {
    sh_decl_value_type type;
    const sh_decl_node *object;
} sh_decl_polymorphic_state;

/* Accept an explicit class with optional object state, or the native scalar
 * class shorthand. Everything is borrowed. NULL/empty/no-op forms remain
 * unavailable because their meaning depends on the destination's prior state.
 * Unsupported forms must stay atomic for composition and explicit dependency
 * gaps; never reinterpret them as ordinary field objects. Returns 1 resolved,
 * 0 unsupported form, -1 an explicit class could not be resolved compatibly. */
int sh_decl_polymorphic_read(const sh_decl_node *node, sh_decl_value_type base_type,
    const sh_decl_dependency_schema *schema, sh_decl_polymorphic_state *state,
    const char **reason);
#endif
