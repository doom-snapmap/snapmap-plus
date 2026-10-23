/* Native reflection adapter for static declaration inspection. */
#ifndef SH_DECL_NATIVE_SCHEMA_H
#define SH_DECL_NATIVE_SCHEMA_H

#include <stdint.h>
#include "decl_dependencies.h"
#include "decl_script_schema.h"

typedef struct sh_decl_native_schema sh_decl_native_schema;

/* Only reads are permitted. The process adapter guards memory access; offline
 * diagnostics can read a frozen image or another process through this boundary.
 * Reflection records must stay stable until close. No reader address is called.
 * read copies exactly length bytes or fails. It need not know object bounds
 * (the process adapter copies any mapped memory), so the schema requests only
 * bytes it knows to exist and reads a string up to its terminator, never past. */
typedef struct sh_decl_native_source {
    void *context;
    int (*read)(void *context, uintptr_t address, void *destination, size_t length);
    uintptr_t reflection;
    const sh_decl_script_schema *scripts; /* immutable, borrowed until close */
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
    int (*reference_site)(void *context, const char *path, sh_decl_value_type type,
        const char *name, size_t name_length, const sh_decl_reference_site *site);
    /* Optional: nonzero while an empty quoted declaration-pointer literal is
     * to be reported as its lookup of _emptyName (empty_lookup). */
    int (*empty_names)(void *context);
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
/* As sh_decl_native_schema_open, but when source is exactly base's (the same
 * read, context, reflection and scripts) the schema reads base's registry and
 * loaded types instead of reading and keeping its own copy; another source, or
 * no base, opens a schema of its own. It keeps its own observer, attached
 * registry and answers. The shared records stay until the last schema reading
 * them closes, in any order, and source must stay readable until then. Schemas
 * sharing records must not be used concurrently. */
/* A settled save scope: between enter (nonzero) and the matching leave, every
 * open over the same source shares one index of the image's reflection
 * registry (sh_decl_native_schema_open_shared). Nested scopes count. */
void sh_decl_native_schema_scope(int enter);
sh_decl_native_schema *sh_decl_native_schema_open_shared(sh_decl_native_schema *base,
    sh_decl_native_source source, sh_decl_dependency_observer observer, char *error, size_t error_capacity);
const sh_decl_dependency_schema *sh_decl_native_schema_view(sh_decl_native_schema *schema);
/* Borrow the declaration registry read from the same image, which proves the
 * manager an event-call decl argument names. Without one those arguments stay
 * gaps. The registry must outlive every walk of this schema. */
struct sh_decl_native_registry;
void sh_decl_native_schema_attach_registry(sh_decl_native_schema *schema,
    struct sh_decl_native_registry *registry);
void sh_decl_native_schema_close(sh_decl_native_schema *schema);
/* Emit every reflected fact typed binding can read, loading each class
 * first: class and enum names with their registered object and pointer
 * readers, each class's super, element and fields, each enum's members.
 * Schemas with equal emissions bind the same text identically. 1 when every
 * emit succeeded; a class or enum that cannot be read is emitted as such. */
int sh_decl_native_schema_digest(sh_decl_native_schema *schema,
    int (*emit)(void *context, const void *bytes, size_t length), void *context);

/* The store of a reflected bitfield setter (variable +0x38), which the
 * bitfield reader (VK 0x141a20600 / GL 0x141a12840) calls with the object
 * itself and the parsed value. 1 when the code is straight-line and only
 * stores width bytes at the object plus offset as (old & ~bits) | value bits
 * that never reach outside bits, then returns: a masked assignment of bits,
 * the whole field, from the value alone. 0 for any other code. */
typedef struct sh_decl_bitfield_store { long long offset; int width; uint64_t bits; } sh_decl_bitfield_store;
int sh_decl_bitfield_setter_store(const unsigned char *code, size_t length, sh_decl_bitfield_store *store);

/* Exact native class spelling and reflected ancestry, without loading fields
 * or constructing objects. Returns 1 derives (including a known class itself),
 * 0 unrelated, -1 unknown/unreadable/cyclic metadata. Unknown equal strings do
 * not establish a valid class. Suitable for the entity class resolver. */
int sh_decl_native_schema_class_derives(sh_decl_native_schema *schema,
    const char *class_name, const char *base_name);
/* Whether the exact name is a reflected type: 1 present, 0 absent, -1 no
 * schema. The index holds the complete reflected registry (an open schema
 * enumerated it to its terminator), so 0 is conclusive for that inventory
 * only. Native entity construction resolves class names through the separate
 * idClass runtime registry (VK 0x1403dd9b0), so absence here does not show
 * that a class cannot be constructed. */
int sh_decl_native_schema_class_known(sh_decl_native_schema *schema, const char *class_name);
/* The reflected type of a member base_name itself does not reflect, as the
 * classes deriving from it that do reflect it read it: 1 every such class
 * gives it the same type (out, borrowed until close), 0 none reflects it,
 * 2 base_name reflects it itself (out cleared), -1 they disagree or the
 * metadata is unknown/unreadable. */
int sh_decl_native_schema_descendant_field(sh_decl_native_schema *schema,
    const char *base_name, const char *key, sh_decl_value_type *out);

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

/* Visit the constants of one registered reflected enum in native order (the
 * record's member array at +0x10, 0x10 stride, name then 32-bit value, NULL
 * name last; the layout the engine's own enum walker reads). Returns 1 after
 * visiting, 0 when no such enum is registered, -1 for unreadable metadata or a
 * failed visit. Names are borrowed until close. */
int sh_decl_native_schema_enum_constants(sh_decl_native_schema *schema, const char *enum_name,
    int (*visit)(void *context, const char *name, uint32_t value), void *context);

typedef enum sh_decl_reference_reader {
    SH_DECL_READER_UNKNOWN,
    SH_DECL_READER_DECL,
    SH_DECL_READER_IMAGE,
    SH_DECL_READER_MODEL,
    SH_DECL_READER_STATIC_MODEL,
    SH_DECL_READER_STATIC_PARTICLE,
    /* idSWF pointers and idSWFFile values construct a UI movie from the field
     * string; the native constructor derives its cooked file/atlas pair. */
    SH_DECL_READER_MOVIE,
    /* idStrRenderModel_Preview values are read as idStr text; SnapMap editor
     * previews pass them to the editor's render-model dispatcher. */
    SH_DECL_READER_PREVIEW_MODEL,
    /* idVoiceTrack pointers find or create the name in the fixed "voicetrack"
     * resource list; an empty name stores NULL. */
    SH_DECL_READER_VOICE_TRACK,
    /* idCollisionModel pointers find or create a name in the fixed "cm"
     * resource list, after rewriting names under maps/. */
    SH_DECL_READER_COLLISION_MODEL
} sh_decl_reference_reader;
/* Select the identity resolver using the registered reader, including types
 * sharing it. Class-name spelling alone does not define a reader contract.
 * Value types qualify only through a registered reference object reader. */
sh_decl_reference_reader sh_decl_native_schema_reader(sh_decl_native_schema *schema,
    sh_decl_value_type type);

#endif
