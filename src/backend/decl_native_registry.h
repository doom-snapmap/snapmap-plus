/* Resolve generic declaration references without loading native resources. */
#ifndef SH_DECL_NATIVE_REGISTRY_H
#define SH_DECL_NATIVE_REGISTRY_H
#include <stddef.h>
#include <stdint.h>

typedef struct sh_decl_native_registry sh_decl_native_registry;
typedef struct sh_decl_registry_source {
    void *context;
    int (*read)(void *context, uintptr_t address, void *destination, size_t length);
    uintptr_t registry;
    /* Optional read-only existence check for the engine's active source mode.
     * 1 present, 0 absent, -1 unavailable. In production mode this must check
     * the effective generated/decls path, not just development source records.
     * Never create defaults, parse declarations or instantiate entities here. */
    int (*source_exists)(void *context, uintptr_t manager, const char *type, const char *name);
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
