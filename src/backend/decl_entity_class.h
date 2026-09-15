/* Resolve an entityDef's gameplay class from a stable source view. */
#ifndef SH_DECL_ENTITY_CLASS_H
#define SH_DECL_ENTITY_CLASS_H

#include "decl_tree.h"

typedef struct sh_decl_entity_class_source {
    void *context;
    /* Read the named parent from this compilation's effective source view.
     * Return 1 with malloc-owned text, 0 absent, -1 unavailable/ambiguous.
     * No native loading/default creation. All callbacks stay stable for a walk.
     * Use a separate original-only view when resolving baseline semantics. */
    int (*read)(void *context, const char *name, sh_decl_source *source);
    /* Exact native class spelling; 1 derives, 0 unrelated, -1 unavailable.
     * Checking a class against itself must still validate its existence. */
    int (*derives)(void *context, const char *class_name, const char *base_name);
} sh_decl_entity_class_source;

/* Own nonempty class overrides the inherited class only if it derives from
 * that class. Empty/absent class inherits; missing class without a parent fails.
 * Missing parents and cycles refuse rather than manufacture native defaults.
 * Traversal uses heap storage and has no authored inheritance-depth quota.
 * Returns a malloc-owned class name, or NULL with an input diagnostic. This
 * resolves the class only, not the parent's effective property values. */
char *sh_decl_entity_class(sh_decl_source definition,
    const sh_decl_entity_class_source *source, char *error, size_t error_capacity);

/* Same operation on a parsed tree, including the composer's scalar metadata
 * projection. The tree stays borrowed and unchanged; parent sources are parsed
 * normally. This avoids serializing/reparsing already validated root metadata. */
char *sh_decl_entity_tree_class(const sh_decl_node *definition,
    const sh_decl_entity_class_source *source, char *error, size_t error_capacity);

/* Expand assignment-style entity edit text from parent to child using the
 * native expanded-state tree contract. Call after validating the class above.
 * Return an owned root containing edit only; no parent state is inspected as
 * an independent entity. Parent reads still establish source dependencies.
 * Ordinary blocks overlay fields, reset markers survive into the final text,
 * and each layer's num removes item indices beyond that count. Unsupported
 * shapes, explicit expandInheritance=false, missing sources and cycles return
 * NULL with a diagnostic. This does not cover custom grammars or later readers.
 * The input stays unchanged; traversal does not consume the C call stack. */
sh_decl_node *sh_decl_entity_expanded_state(const sh_decl_node *definition,
    const sh_decl_entity_class_source *source, char *error, size_t error_capacity);

#endif
