/* Typed serialized gameplay resource roots; editorVars subtrees are excluded.
 * Dependency expansion is supplied separately
 * by resource-family readers; arbitrary map strings are never asset roots. */
#ifndef SH_PACKAGE_USAGE_H
#define SH_PACKAGE_USAGE_H
#include "package_compiler.h"
#include "resource_catalog.h"
typedef int (*sh_package_reference_visitor)(void *context, const char *type, const char *name);
/* Instance-local references resolved through the native field readers before
 * acquiring a compiler snapshot. They never modify a shared asset's graph. */
typedef struct sh_package_reference { char *type, *name; } sh_package_reference;
typedef struct sh_package_references {
    sh_package_reference *items;
    size_t count;
    int incomplete;
} sh_package_references;
int sh_package_references_add(void *references, const char *type, const char *name);
void sh_package_references_free(sh_package_references *references);
typedef int (*sh_package_state_visitor)(void *context, const char *class_name,
    const char *inherit, const char *edit, size_t length);
int sh_package_map_states(const char *json, size_t length,
    sh_package_state_visitor visitor, void *context);
int sh_package_map_references(const char *json, size_t length,
                              sh_package_reference_visitor visitor, void *context);
/* Select roots and their known gameplay dependencies. Coverage is separate
 * from success: missing graph records do not erase known owners or make a
 * vanilla map fail. An interrupted walk or invalid JSON returns 0 and clears
 * both outputs. Initialize owner sets to zero and release them with
 * sh_package_owners_free; repeat calls replace their contents. The caller holds a stable compilation; no engine calls occur. */
int sh_package_map_owners(const sh_package_compilation *compiled,
                               const sh_resource_catalog *catalog,
                               const char *json, size_t length,
                               const sh_package_references *references, sh_package_owners *owners,
                               int *dependencies_complete);
/* Resolve known gameplay roots and recorded edges to canonical provider paths.
 * Unlike delivery ownership, this retains required paths not supplied by a map
 * package so availability can detect an undeliverable dependency. Output items
 * have empty type and canonical name. Initialize out to zero; failure clears it.
 * out.incomplete describes recorded graph/inline coverage only. The caller must
 * also establish that those observations describe the candidate map's bytes;
 * a graph captured from different local resource content is not that proof. */
int sh_package_map_resources(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, const char *json, size_t length,
    const sh_package_references *references, sh_package_references *out);

/* Use exactly the same automatic ownership as whole-package delivery. This
 * includes policies in every nested component of a selected delivery owner.
 * An editor-only/vanilla map produces an empty policy, not installed policy.
 * Partial dependency coverage remains explicit. Failure clears both outputs;
 * release a prior policy with sh_package_policy_free before calling again. */
int sh_package_map_policy(const sh_package_compilation *compiled,
                            const sh_resource_catalog *catalog,
                            const char *json, size_t length, const sh_package_references *references,
                            sh_package_policy *out, sh_package_owners *owners,
                            int *dependencies_complete,
                            char *error, size_t error_capacity);
#endif
