/* Native reflection adapter for static declaration inspection. */
#ifndef SH_DECL_NATIVE_SCHEMA_H
#define SH_DECL_NATIVE_SCHEMA_H

#include <stdint.h>
#include "decl_dependencies.h"

typedef struct sh_decl_native_schema sh_decl_native_schema;

/* Only reads are permitted. The process adapter guards memory access; offline
 * diagnostics can read a frozen image or another process through this boundary.
 * Reflection records must stay stable until close. No reader address is called. */
typedef struct sh_decl_native_source {
    void *context;
    int (*read)(void *context, uintptr_t address, void *destination, size_t length);
    uintptr_t reflection;
} sh_decl_native_source;

/* Locate the already-initialized reflection context through a signature-bound
 * declaration-manager accessor without invoking it. Both native LEAs must name
 * the same manager, the accessor epilogue must match, and reflection must point
 * at the separately verified type container. Returns 1 ready, 0 not initialized,
 * -1 unavailable/inconsistent. Failure clears reflection. No engine writes. */
int sh_decl_native_source_bind(sh_decl_native_source *source,
    uintptr_t accessor, uintptr_t type_container);

/* The GameSystem initializer finishes reader registration before adding the
 * first checkpoint stream. Its exported getter only returns the static system
 * object. Decode it without calling native code and require that initialized
 * list before binding reflection. This distinguishes allocated reader tables
 * from completed registration. Returns 1 ready, 0 initializing, -1 invalid.
 * Source readers must run before system shutdown, as with schema_open. */
int sh_decl_native_source_ready(sh_decl_native_source *source,
    uintptr_t accessor, uintptr_t type_container, uintptr_t game_system_export);

typedef struct sh_decl_dependency_observer {
    void *context;
    int (*reference)(void *context, const char *path, sh_decl_value_type type,
                     const char *name, size_t name_length);
    void (*gap)(void *context, const char *path, sh_decl_value_type type, const char *reason);
} sh_decl_dependency_observer;

/* Captures registry identities, then reads fields on demand. Custom readers
 * are recognized through their native registrations, never template-name
 * parsing alone. Unsupported readers and failed field reads remain gaps.
 * This covers typed deserialization; later gameplay consumers of string data
 * still require their own dependency adapters. The observer may be empty for
 * composition/metadata-only inspection; dependency emission then aborts rather
 * than silently treating an unobserved reference as complete. */
sh_decl_native_schema *sh_decl_native_schema_open(sh_decl_native_source source,
    sh_decl_dependency_observer observer, char *error, size_t error_capacity);
const sh_decl_dependency_schema *sh_decl_native_schema_view(sh_decl_native_schema *schema);
void sh_decl_native_schema_close(sh_decl_native_schema *schema);

/* Exact native class spelling and reflected ancestry, without loading fields
 * or constructing objects. Returns 1 derives (including a known class itself),
 * 0 unrelated, -1 unknown/unreadable/cyclic metadata. Unknown equal strings do
 * not establish a valid class. Suitable for the entity class resolver. */
int sh_decl_native_schema_class_derives(sh_decl_native_schema *schema,
    const char *class_name, const char *base_name);

/* Resolve a canonical declaration family to the reflected edit-state class.
 * The verified type/class bindings and native idDeclTypeInfo ancestry establish
 * this contract, including game declarations and their post-parse wrappers.
 * Entity definitions select an entity class separately. Custom native grammars
 * such as material and MD6 are not reflected edit-state readers.
 * Returns 1 resolved, 0 other/unknown family, -1 missing or invalid metadata.
 * Output strings are borrowed until close; every failure clears the output. */
int sh_decl_native_schema_decl_type(sh_decl_native_schema *schema,
    const char *declaration_type, sh_decl_value_type *state_type);

/* Recognize the shared graph envelope by both reflected ancestry and its
 * registered object reader. No renderer address or family-name heuristic.
 * Returns 1 graph, 0 other type/indirection, -1 unavailable/inconsistent. */
int sh_decl_native_schema_graph_type(sh_decl_native_schema *schema, sh_decl_value_type type);

typedef enum sh_decl_reference_reader {
    SH_DECL_READER_UNKNOWN,
    SH_DECL_READER_DECL,
    SH_DECL_READER_IMAGE,
    SH_DECL_READER_MODEL
} sh_decl_reference_reader;
/* Select the identity resolver using the registered reader, including types
 * sharing it. Class-name spelling alone does not define a reader contract. */
sh_decl_reference_reader sh_decl_native_schema_reader(sh_decl_native_schema *schema,
    sh_decl_value_type type);

#endif
