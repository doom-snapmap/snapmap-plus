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
    /* Optional: 1 a reflected class, 0 absent from the complete reflected
     * inventory, -1 unavailable (sh_decl_native_schema_class_known). */
    int (*known)(void *context, const char *class_name);
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
 * and each layer's num removes item indices beyond that count. A definition
 * stating its own expandInheritance false (sh_decl_entity_expands) prepares
 * its own state text alone: no parent is read, merged or post-passed, while
 * its class and editor variables still come from the chain (sh_decl_entity_
 * tree_class). An ancestor's flag never applies to an inheriting definition,
 * so a placed copy of an unexpanded definition still merges its whole chain.
 * Unsupported shapes, a non-boolean flag, missing sources and cycles return
 * NULL with a diagnostic. This does not cover custom grammars or later readers.
 * The input stays unchanged; traversal does not consume the C call stack. */
sh_decl_node *sh_decl_entity_expanded_state(const sh_decl_node *definition,
    const sh_decl_entity_class_source *source, char *error, size_t error_capacity);

/* How definition's own state text merges into its inheritance chain: the
 * per-layer merge (VK 0x141a65560 / GL 0x141a57a30) passes
 * *tree != 0, so it updates by first match once some ancestor's state text
 * has added a node, and appends every occurrence before that. Headers are
 * positional (sh_decl_entity_split). Returns 1 update mode (the root-most
 * stateful ancestor's first state statement, which SetEntityEditState reads,
 * is a block assignment; its name, malloc-owned in state_name when given, is
 * the one later layers reach by first match), 0 first stateful layer, -1
 * unavailable, a cycle, or an inherited first statement outside that model.
 * A definition without a positional parent, or stating its own
 * expandInheritance false, needs no source and returns 0. */
int sh_decl_entity_merges(const sh_decl_node *definition,
    const sh_decl_entity_class_source *source, char **state_name, char *error, size_t error_capacity);
/* Why the merge above is undecided (-1). None is a native representation
 * limit: malformed input (an unsupported header value or resource name, a
 * cycle, an unreadable parent text), a conclusively absent parent (the
 * reader's 0), an inherited first statement outside the verified merge model
 * (remaining product coverage), allocation, or a parent source the reader
 * could not supply (no reader, or its -1: unavailable or ambiguous), which is
 * no evidence about the definition. Callers must not substitute a state. */
typedef enum sh_decl_entity_merge_failure {
    SH_DECL_ENTITY_MERGE_DECIDED = 0,
    SH_DECL_ENTITY_MERGE_MALFORMED,
    SH_DECL_ENTITY_MERGE_MISSING,
    SH_DECL_ENTITY_MERGE_COVERAGE,
    SH_DECL_ENTITY_MERGE_ALLOCATION,
    SH_DECL_ENTITY_MERGE_UNAVAILABLE
} sh_decl_entity_merge_failure;
int sh_decl_entity_merges_classified(const sh_decl_node *definition,
    const sh_decl_entity_class_source *source, char **state_name, sh_decl_entity_merge_failure *failure,
    char *error, size_t error_capacity);

/* sh_decl_entity_tree_class with why it failed (failure untouched on
 * success). A cycle, an unsupported header or resource name, or a layer class
 * that native Parse rejects against its parent's class (reflected ancestry;
 * an unreflected class differing from it fails) is MALFORMED; a conclusively
 * absent parent MISSING; a parent or class metadata the source cannot read
 * UNAVAILABLE. A parentless class absent from the reflected inventory is
 * COVERAGE: Parse accepts it and construction resolves it through the idClass
 * registry, which this does not read. */
char *sh_decl_entity_tree_class_classified(const sh_decl_node *definition,
    const sh_decl_entity_class_source *source, sh_decl_entity_merge_failure *failure,
    char *error, size_t error_capacity);
/* The same; on COVERAGE, *unreflected_class (when given) receives the
 * malloc-owned effective class spelling, which construction resolves through
 * the idClass registry rather than reflection. NULL otherwise. */
char *sh_decl_entity_tree_class_detailed(const sh_decl_node *definition,
    const sh_decl_entity_class_source *source, sh_decl_entity_merge_failure *failure,
    char **unreflected_class, char *error, size_t error_capacity);

#endif
