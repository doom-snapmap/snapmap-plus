/* Resolve generic declaration references without loading native resources. */
#ifndef SH_DECL_NATIVE_REGISTRY_H
#define SH_DECL_NATIVE_REGISTRY_H
#include <stddef.h>
#include <stdint.h>

typedef struct sh_decl_native_registry sh_decl_native_registry;
struct sh_decl_script_schema;
typedef struct sh_decl_registry_source {
    void *context;
    int (*read)(void *context, uintptr_t address, void *destination, size_t length);
    uintptr_t registry;
    /* Optional read-only existence check for the engine's active source mode.
     * 1 present, 0 absent, -1 unavailable. In production mode this must check
     * the effective generated/decls path, not just development source records.
     * Never create defaults, parse declarations or instantiate entities here. */
    int (*source_exists)(void *context, uintptr_t manager, const char *type, const char *name);
    const struct sh_decl_script_schema *scripts; /* borrowed immutable metadata */
} sh_decl_registry_source;

typedef struct sh_decl_reference {
    const char *type;       /* borrowed until registry close */
    char *name;             /* owned; release with sh_decl_reference_clear */
    uintptr_t resource;    /* existing object, zero for a source-only result */
} sh_decl_reference;

/* Copies native registration order and manager ancestry. Loaded identities
 * are copied on first use. Call at a stable engine boundary and close before
 * registration, loading or unloading changes this snapshot. No native code
 * is invoked. Counts bound native arrays, not authored package payloads. */
sh_decl_native_registry *sh_decl_native_registry_open(sh_decl_registry_source source,
    char *error, size_t error_capacity);
void sh_decl_native_registry_close(sh_decl_native_registry *registry);

/* Resolve a serialized declaration type to its registered native class using
 * copied metadata only. Does not inspect or load declaration objects. Returns
 * 1 found, 0 absent, -1 ambiguous/invalid; class is borrowed until close. */
int sh_decl_native_registry_type_class(sh_decl_native_registry *registry,
    const char *type, const char **class_name);
/* The inverse: the first manager in native order whose class is exactly this
 * one, which the typed declaration pointer reader selects. 1 found, 0 none
 * (the reader then searches every manager), -1 invalid. Borrowed until close. */
int sh_decl_native_registry_class_type(sh_decl_native_registry *registry,
    const char *class_name, const char **type);

enum sh_native_resource_identity_result {
    SH_NATIVE_RESOURCE_ID_ERROR = -1,
    SH_NATIVE_RESOURCE_ID_UNREGISTERED = 0,
    SH_NATIVE_RESOURCE_ID_FREE = 1,
    SH_NATIVE_RESOURCE_ID_SAME_NAME = 2,
    SH_NATIVE_RESOURCE_ID_COLLISION = 3
};
typedef struct sh_native_resource_identity {
    const char *type;          /* copied native spelling; borrowed until close */
    char *name;                /* owned normalized candidate name */
    uint32_t id, resident_id;  /* prospective ID; stored ID when occupied */
    uintptr_t manager, resource;
    const char *resident_name; /* copied occupant name; borrowed until close */
} sh_native_resource_identity;

/* Read-only identity checks for a logical, ASCII-insensitive family. IDs use
 * the exact registered type spelling, never the logical spelling supplied by
 * the caller. A different-name ID occupant takes precedence over a same-name
 * occupant. SAME_NAME establishes identity occupancy only, not equal payloads,
 * safe replacement, or authorization to reuse a resident object.
 *
 * Loaded IDs/names are captured and verified on first query of each manager;
 * subsequent queries use the copied index at the required stable boundary.
 * Call identity_validate after the complete allocation batch and before
 * publication to reject changed membership, names, owners, IDs or metadata.
 * No native/source-existence function is called. Unknown or inconsistent IDs
 * fail closed. Output must be empty; failures leave it empty. This decl view
 * covers exactly its supplied registration array, without extra class filters. */
int sh_decl_native_registry_identity(sh_decl_native_registry *registry,
    const char *family, const char *name, sh_native_resource_identity *out,
    char *error, size_t capacity);
int sh_decl_native_registry_identity_validate(sh_decl_native_registry *registry,
    char *error, size_t capacity);
void sh_native_resource_identity_clear(sh_native_resource_identity *identity);

/* Identity-only view of the complete native linked manager registry. head_slot
 * is the ADDRESS OF THE POINTER that stores the first manager, not the first
 * manager itself. Next links are at manager+0x18. This view reads no declaration
 * class/parent fields and never changes declaration resolution order. The same
 * stable-boundary, query/validate, result and ownership contracts above apply. */
typedef struct sh_native_resource_registry sh_native_resource_registry;
typedef struct sh_native_resource_registry_source {
    void *context;
    int (*read)(void *context, uintptr_t address, void *destination, size_t length);
    uintptr_t head_slot;
} sh_native_resource_registry_source;
sh_native_resource_registry *sh_native_resource_registry_open(sh_native_resource_registry_source source,
    char *error, size_t capacity);
void sh_native_resource_registry_close(sh_native_resource_registry *registry);
int sh_native_resource_registry_identity(sh_native_resource_registry *registry,
    const char *family, const char *name, sh_native_resource_identity *out,
    char *error, size_t capacity);
int sh_native_resource_registry_validate(sh_native_resource_registry *registry,
    char *error, size_t capacity);
/* Revalidate only what one identity query read: its manager header and the
 * entries sharing its ID. For repeated queries of one stable snapshot; the
 * owner must still run the full validate before relying on the whole set. */
int sh_native_resource_registry_validate_identity(sh_native_resource_registry *registry,
    const sh_native_resource_identity *identity, char *error, size_t capacity);

/* Only for the generic declaration-pointer reader; image and render-model
 * readers have different contracts. Returns 1 resolved, 0 absent, -1 unknown.
 * Class matching is ASCII-insensitive. A matching manager searches its loaded
 * objects, then loaded descendants, then its source and descendant sources.
 * Without a class match, that lookup runs for every manager in native order.
 * Unknown source availability stops the search rather than selecting a later
 * candidate. This never manufactures the native make-default fallback.
 * Output must be empty on entry. Failures leave it empty. */
int sh_decl_native_registry_resolve(sh_decl_native_registry *registry,
    const char *class_name, const char *name, sh_decl_reference *reference);
/* Resolve a prospective compilation through source_exists only. Native
 * registration order and ancestry still define the candidate families, but
 * previously loaded local objects cannot establish candidate availability or
 * choose a different family. Never reads native resource arrays. This is for
 * source analysis, not a prediction of a stale engine lookup before activation.
 * A successful result has resource=0. The same return/ownership contract applies. */
int sh_decl_native_registry_resolve_source(sh_decl_native_registry *registry,
    const char *class_name, const char *name, sh_decl_reference *reference);

/* Candidate discovery only: enumerate the managers that the native declaration
 * pointer reader can search, without requiring a source or resident object.
 * Names of absent/tombstoned resources remain boundaries in a reverse graph.
 * This is not a binding or an admission certificate. Callback returns 1 to
 * continue, 0 to abort; names/types are borrowed only during the call. */
typedef int (*sh_decl_source_candidate)(void *context, const char *family, const char *name);
int sh_decl_native_registry_source_candidates(sh_decl_native_registry *registry,
    const char *class_name, const char *name, sh_decl_source_candidate candidate, void *context);
void sh_decl_reference_clear(sh_decl_reference *reference);

typedef struct sh_decl_prepared_state {
    char *class_name, *text;
    size_t length;
    int expanded_inheritance;
} sh_decl_prepared_state;
/* Copy an existing entityDef's prepared state. Pending/default/error objects
 * and changing metadata are unavailable. Output must be empty on entry. */
int sh_decl_native_registry_entity_state(sh_decl_native_registry *registry,
    const sh_decl_reference *reference, sh_decl_prepared_state *state);
void sh_decl_prepared_state_clear(sh_decl_prepared_state *state);
#endif
