/* Local authoring compilation: shared editor state and isolated asset scopes. */
#ifndef SH_PACKAGE_LIBRARY_H
#define SH_PACKAGE_LIBRARY_H
#include "package_source_link.h"
#include "package_virtual_material.h"
#include "package_strings.h"
#include "package_consumers.h"
#include "package_audio_program.h"

typedef struct sh_package_library_native {
    sh_package_source_link_native source;
    /* Resolve the effective native page provider from the scoped landPageFile
     * value. Empty means the native default. Supply complete immutable page
     * identity and the actual provider/source names; never infer these from a
     * currently loaded material. Borrowed fields may point into page_file or
     * context-owned storage and are copied immediately after this callback.
     * Return 1 for a complete binding, 0 unsupported, -1 failed read. */
    int (*atlas)(void *context, size_t scope, const sh_package_compilation *compiled,
        const char *material, const char *page_file, sh_package_virtual_atlas *out,
        char *error, size_t capacity);
    /* Required for native publication; offline fixtures may omit this. This is
     * the complete resource-manager list, not just declaration managers. */
    sh_native_resource_registry_source identities;
    /* A caller-owned native dictionary reservation transaction. Required only
     * when verified typed sites acquire private keys. Keep reservations through
     * activation/revalidation; discard them on any later candidate failure. */
    sh_package_string_reservations strings;
    /* Captured locale/original bank inventory. The owning provider retains it
     * beyond the returned audio program's lifetime. Native activation remains
     * separate from compilation and must publish the complete prepared recipe. */
    sh_audio_originals *audio;
} sh_package_library_native;

typedef struct sh_package_library {
    sh_package_compilation *compiled;
    /* Complete local binding index. A map selects only the bindings it uses
     * before serializing provenance; never embed this entire index. */
    sh_package_provenance bindings;
    /* Transfer this unpublished handle into the provider before activation. */
    sh_virtual_material_view *virtual_materials;
    /* Immutable constructor requests, retained with this provider's files. */
    sh_package_consumers *consumers;
    sh_package_audio_program *audio;
    /* Owned text accounting missing product coverage that did not refuse the
     * library, one line per item; NULL when coverage was complete. */
    char *coverage;
} sh_package_library;

/* Compile lexical component scopes before comparing any peer gameplay edits.
 * Typed source bindings are captured before registered editor fields compose.
 * Inspector lists compose before controls, so control equality uses the resulting
 * bound inspector behavior. Per-field target indices survive projection into
 * every private variant; private gameplay fields never merge with peer fields.
 * Final graph equality and native assignment run after the contribution phases.
 * The native adapter still supplies implicit-state certificates. No filesystem
 * mutations, native publication, installation, or map parsing occurs here.
 * Inventory and native metadata remain stable during this synchronous build.
 * Empty libraries compile product defaults normally. Legacy maps use their
 * recorded composed compiler, not this local-library entry point.
 * Initialize out to zero; replacement/failure clears it. */
int sh_package_library_compile(const sh_package_sources *sources,
    const sh_package_compile_environment *environment,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_library_native *native, sh_package_library *out,
    char *error, size_t capacity);
void sh_package_library_free(sh_package_library *library);

/* Reconstruct only the map's recorded contexts from its private inventory.
 * Isolated contexts keep lexical lookup; historical composed contexts retain
 * their recorded complete owner set and composition rules. Current local
 * packages never fill missing trees. The returned binding index maps original
 * names/context keys to the newly compiled names for typed map rebinding. */
int sh_package_library_compile_contexts(const sh_package_sources *sources,
    const sh_package_provenance *contexts, const sh_package_compile_environment *environment,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_library_native *native, sh_package_library *out,
    char *error, size_t capacity);
#endif
