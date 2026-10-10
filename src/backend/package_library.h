/* Local authoring compilation: shared editor state and isolated asset scopes. */
#ifndef SH_PACKAGE_LIBRARY_H
#define SH_PACKAGE_LIBRARY_H
#include "package_source_link.h"
#include "package_virtual_material.h"
#include "package_strings.h"
#include "package_consumers.h"
#include "package_audio_program.h"
#include "package_keys.h"
#include "package_fsm.h"

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
    /* The backend's spawn-key detours are installed (spawn_keys.c): package
     * registrations take private keys (package_keys.h). 0 keeps native bits. */
    int private_keys;
    /* The registration keys a peer library holds (its spawnaitype bindings).
     * A scope of the same source tree and component keeps that key, so a map
     * compile serves installed packages exactly as the installed library does
     * and a recompile keeps live keys; other scopes take keys it leaves free. */
    const sh_package_provenance *keys;
    /* The same peer's group codes (package_keys.h): an equal set keeps its
     * code, and no other set takes one of them. */
    const sh_package_key_groups *key_groups;
    /* Shared process-lifetime native binding/alias reservations. */
    sh_package_link_history *history;
    /* A legacy map's own typed values. Its historical composed contexts then
     * derive exactly those stock roots their sources change (a bank's sound,
     * an entity reaching a changed material or model); nothing else is
     * discovered, and only the document's sites select any of them. */
    const sh_package_references *document_roots;
    /* SnapMap's shipped editor entries (names and parents; package_editor_rows.h),
     * read only when a package leaves a stock entry's inherited controls at
     * other slots: every stock descendant of that entry is then rebased onto
     * it. 1 with every entry read and *rows set; 0 unavailable or -1 when an
     * entry could not be read, either of which fails that compile as an
     * environment failure (no partial descendant closure is served). NULL
     * (offline tools) reads none. */
    int (*editor_rows)(void *context, const struct sh_editor_rows **rows);
    void *editor_rows_context;
} sh_package_library_native;

/* Build phases timed for the activation log. */
enum {
    SH_PACKAGE_LIBRARY_PHASE_SCOPES, SH_PACKAGE_LIBRARY_PHASE_NATIVE, SH_PACKAGE_LIBRARY_PHASE_SOURCE_LINK,
    SH_PACKAGE_LIBRARY_PHASE_CONTRIBUTIONS, SH_PACKAGE_LIBRARY_PHASE_IDENTITIES, SH_PACKAGE_LIBRARY_PHASE_LINK,
    SH_PACKAGE_LIBRARY_PHASE_OUTPUTS, SH_PACKAGE_LIBRARY_PHASE_AUDIO, SH_PACKAGE_LIBRARY_PHASES
};

/* An authored shader input the source link accounted without publishing it
 * (sh_package_shader_unpublished): file indexes the compiled sources. A dormant
 * pair also names the binary whose checksum matched: pair indexes the sources,
 * or is SIZE_MAX when pair_path is the installed SnapMap original. */
typedef struct sh_package_unpublished_shader {
    size_t file, pair;
    char *pair_path;
    int reason, renderer;   /* sh_package_shader_unpublished_reason; served renderer or -1 */
    uint32_t checksum;
} sh_package_unpublished_shader;

typedef struct sh_package_library {
    sh_package_compilation *compiled;
    /* Complete local binding index. A map selects only the bindings it uses
     * before serializing provenance; never embed this entire index. */
    sh_package_provenance bindings;
    /* Transfer this unpublished handle into the provider before activation. */
    sh_virtual_material_view *virtual_materials;
    /* Immutable constructor requests, retained with this provider's files. */
    sh_package_consumers *consumers;
    /* AI state-machine webs this provider binds and the state-machine
     * declarations it owns (package_fsm.h); NULL when nothing binds or is owned. */
    sh_package_fsm_webs *fsm_webs;
    sh_package_audio_program *audio;
    /* Owned group codes the relocated values use (NULL when none): the spawn
     * key detours need them to read those values; publish with the provider. */
    sh_package_key_groups *key_groups;
    /* Owned text accounting missing product coverage that did not refuse the
     * library, one line per item; NULL when coverage was complete. After a
     * failed compile it may remain set, as a diagnostic, until library_free. */
    char *coverage;
    /* Wall-clock milliseconds of each build phase in the last compile; like
     * coverage it survives a failed compile, as a diagnostic only. */
    unsigned long long phase_ms[SH_PACKAGE_LIBRARY_PHASES];
    char source_phases[256]; /* The source capture's own phases, when it opened. */
    char contribution_phases[256]; /* The contribution phase's own steps, when it ran. */
    /* Diagnostic repeated-work lines of the last compile, '\n'-separated:
     * parse census by phase, source capture repeats, identity passes. */
    char repeated_work[8192];
    /* Owned developer report of a refused native identity (newline-separated
     * lines naming both claimants), NULL when none; like coverage it survives
     * a failed compile, as a diagnostic only. */
    char *link_diagnostics;
    /* Owned per-input shader accounting of a linked library. Diagnostics only:
     * shader_inputs_lost reports records that could not be kept, never a
     * reason to refuse the library. */
    sh_package_unpublished_shader *shader_inputs;
    size_t shader_input_count;
    int shader_inputs_lost;
} sh_package_library;

/* Write "scopes N ms, native N ms, ..." for the last compile; returns out. */
const char *sh_package_library_phases(const sh_package_library *library, char *out, size_t capacity);

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
/* Compile every package of sources as one composed scope, as a map without
 * recorded contexts composes its carried packages: their copies of the stock
 * player hands web align there package by package, as in a composed context.
 * Without a baseline reader no stock contract is known and none aligns. */
sh_package_compilation *sh_package_library_compile_composed(const sh_package_sources *sources,
    const sh_package_compile_environment *environment, char *error, size_t capacity);
#ifdef SH_PACKAGE_LIBRARY_TESTING
/* Fail the next count editor contribution reads of one declaration name
 * (any name when NULL) as an unreadable source would. */
void sh_package_library_test_contribution_read_failures(const char *name, long count);
/* Fail the next count compositions that isolate a combination gap's
 * participant as an environment failure (allocation) would. */
void sh_package_library_test_probe_failures(long count);
/* 0 composes every editor contribution member afresh instead of reusing the
 * body of an earlier member with the same exact input (the same result). */
void sh_package_library_test_editor_memo(int enabled);
/* 0 rewrites the registration keys of every declaration afresh instead of
 * reusing each binding class's scanned key sites (the same result). */
void sh_package_library_test_key_memo(int enabled);
/* Fail the next count re-judgements of a group that found a package as an
 * allocation failure would. */
void sh_package_library_test_rejudge_failures(long count);
/* Fail the next count token rebinds after a package was found, as a failure
 * no judgement classifies. */
void sh_package_library_test_token_failures(long count);
/* Judge the contributions of the named packages (up to 8; NULL or 0 clears;
 * "package:declaration" names one declaration only) malformed where their
 * editor or registration group composes, as an invalid source found at that
 * phase would be. */
void sh_package_library_test_contribution_defects(const char *const *packages, size_t count);
#endif
#endif
