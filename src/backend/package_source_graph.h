/* Dependency inspection of a retained candidate's declaration bytes. */
#ifndef SH_PACKAGE_SOURCE_GRAPH_H
#define SH_PACKAGE_SOURCE_GRAPH_H

#include "package_usage.h"
#include "decl_native_registry.h"
#include "decl_native_schema.h"
#include "package_provenance.h"
#include "package_shader.h"
#include "audio_originals.h"

typedef struct sh_package_source_graph_report {
    size_t declarations, references, gaps;
    char first_gap[1024];
} sh_package_source_graph_report;

/* Inspect candidate bytes without publishing them or changing the observed
 * native graph. The original reader must bypass live overrides. The caller
 * supplies the retained authored compilation and stable native
 * metadata. Known paths survive unsupported readers; incomplete stays explicit.
 * This resolves explicit source references and supported expanded entity state,
 * including overridden parent fields and truncated inherited lists. Verified
 * sound-name conversion selects candidate bank event memberships, retaining
 * every matching locale path and each path's existing delivery ownership.
 * Installed/packed audio and streamed media still need their provider indexes.
 * Other custom grammars, unexpanded inheritance and late gameplay string consumers still
 * require adapters. It cannot alone certify engine closure.
 * Output starts zeroed and is replaced on success; failure clears it. */
int sh_package_source_graph(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const char *json, size_t length, sh_package_references *out,
    sh_package_source_graph_report *report, char *error, size_t capacity);

typedef struct sh_package_source_reader sh_package_source_reader;
typedef struct sh_package_source_audio {
    sh_audio_originals *originals; /* borrowed immutable provider lifetime */
} sh_package_source_audio;
typedef struct sh_package_source_shader {
    sh_package_shader_renderer renderer;
    void *context;
    /* Prove the exact source-bound parameter can retain the native/GPU slot.
     * source.text is NULL only after physical source absence was captured.
     * Return 1 with verified kind (0..18) and native index (0..32767), 0 for
     * missing/incompatible coverage, -1 on failed read. Same-name occupancy or
     * equal freshness hashes alone are insufficient. The adapter must retain
     * stable native kind/index, source/default/layout compatibility and lifetime
     * through publication/rollback; it must not use a peer's loaded defaults.
     * Source-free acceptance requires a positively established native origin.
     * Callback/context are borrowed for the reader's lifetime. */
    int (*parameter)(void *context, const char *name, sh_decl_source source,
        int *kind, uint32_t *index, char *error, size_t capacity);
    /* Cooked define records resolve renderProgFlag declarations on source
     * fallback. Verify the exact source and fixed native 64-bit mask. */
    int (*flag)(void *context, const char *name, sh_decl_source source,
        uint64_t *mask, char *error, size_t capacity);
    /* Missing OpenGL cooked output may require admitted native source cooking.
     * This read-only callback retains an environment request; it never invokes
     * the engine. The caller retries the same immutable scope after preparation. */
    int (*program)(void *context, const char *name, sh_decl_source source,
        char *error, size_t capacity);
} sh_package_source_shader;
/* Current renderer graph route. The legacy entry point above retains an
 * explicit coverage gap when it encounters a shader without this contract. */
int sh_package_source_graph_with_shader(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_source_shader *shader, const char *json, size_t length,
    sh_package_references *out, sh_package_source_graph_report *report,
    char *error, size_t capacity);
/* Families whose cooked payload names no other engine resource (image,
 * skeleton, collision and similar), measured over the installed corpus. */
int sh_package_source_leaf_family(const char *family);
/* The library link's admission of a verified engine-created identity with no
 * physical source, such as an intrinsic image. It is asked only after no
 * catalogued or supplied file exists; 1 admits, 0 is unsupported, -1 fails.
 * Callback/context are borrowed for the walk. */
typedef struct sh_package_source_admission {
    void *context;
    int (*admit)(void *context, size_t scope, const char *family, const char *name,
        unsigned char semantics[32], char *error, size_t capacity);
} sh_package_source_admission;
int sh_package_source_graph_with_contracts(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_source_shader *shader, const sh_package_source_audio *audio,
    const sh_package_source_admission *admission,
    const char *json, size_t length, sh_package_references *out,
    sh_package_source_graph_report *report, char *error, size_t capacity);
/* Keep one immutable scope and native metadata snapshot through both the graph
 * and relocation passes. It never observes another package's loaded objects. */
sh_package_source_reader *sh_package_source_reader_open(const sh_package_compilation *compiled,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    char *error, size_t capacity);
void sh_package_source_reader_close(sh_package_source_reader *reader);
/* Optional only for scopes that never reach a renderprog. Once configured,
 * renderer and ABI contract are copied; caller-owned callback state stays live. */
int sh_package_source_reader_shader(sh_package_source_reader *reader,
    const sh_package_source_shader *shader);
int sh_package_source_reader_audio(sh_package_source_reader *reader,
    const sh_package_source_audio *audio);
/* Decode the selected renderer's cooked program with scoped parameter kinds.
 * Validate every retained fixed parameter ABI and the declaration checksum.
 * All returned bytes/program arrays are owned by the caller. */
int sh_package_source_shader_open(sh_package_source_reader *reader, const char *name,
    unsigned char **declaration, size_t *declaration_length,
    unsigned char **binary, size_t *binary_length, sh_package_shader_program *program,
    char *error, size_t capacity);
/* Bind one member of a complete shader unit. Original .decl/.bin/stage/include
 * files remain coherent: binary freshness uses the same bound declaration,
 * while exact stage/include bytes retain their original ABI and timestamps. */
unsigned char *sh_package_source_shader_bind(sh_package_source_reader *reader,
    const char *name, const char *path, const unsigned char *body, size_t length,
    int (*resolver)(void *, const char *, const char *, const char *, const char **),
    void *context, size_t *output_length, char *error, size_t capacity);

/* Resolve a String render parameter from its scoped default and ordered
 * material writes. This path-valued projection accepts one native token (or
 * an empty String), including quoted paths. It does not inspect live objects
 * or interpret arbitrary String values as resource references. Owned result. */
char *sh_package_source_material_path(sh_package_source_reader *reader,
    const char *material, const char *parameter, char *error, size_t capacity);

/* A binding is identified by its verified field or native record slot, resource
 * family and manager name. Return 0 to keep, 1 with a borrowed replacement name,
 * or negative to abort. Model fields retain the engine's dispatcher suffix. */
typedef int (*sh_package_binding_resolver)(void *context, const char *slot,
    const char *family, const char *name, const char **replacement);
typedef struct sh_package_source_binding {
    size_t references, changed;
    sh_package_source_graph_report inspection;
    /* The declaration text itself does not parse in the native grammar. A
     * missing reader adapter or unresolved reference is not malformation. */
    int malformed;
} sh_package_source_binding;
/* Bind one supported source envelope. NULL on incomplete grammar or failed
 * resolution; output is an owned exact copy for a no-op. No source is changed.
 * This establishes explicit source bindings, not later code-driven lookups or
 * native ABI compatibility. A linker must also account for dependencies implied
 * by resource names, catalog permutations and renderer/audio contracts. */
unsigned char *sh_package_source_bind(sh_package_source_reader *reader,
    const char *family, const char *name, const unsigned char *body, size_t length,
    sh_package_binding_resolver resolver, void *context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity);

/* Reverse candidate discovery uses the same source grammar, but declaration
 * pointers enumerate their native search managers without requiring a target
 * source. Missing names remain graph boundaries, never admitted bindings.
 * Observers must return zero (keep) or negative (abort); success is an exact
 * source copy. Final selected units must still pass ordinary source binding. */
unsigned char *sh_package_source_observe(sh_package_source_reader *reader,
    const char *family, const char *name, const unsigned char *body, size_t length,
    sh_package_binding_resolver observer, void *context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity);

/* A projected collection may contain an already-bound target from another
 * immutable scope. Admit only such captured identities before native reference
 * decoding asks whether they exist. Return 1 and set exists to 0/1, 0 to use the
 * source scope, or -1 to abort. This never supplies bytes, class metadata or
 * edge targets; the ordinary resolver still binds every observed typed slot.
 * The hook is borrowed for this synchronous call and cannot reenter reader. */
typedef int (*sh_package_bound_exists)(void *context, const char *family,
    const char *name, int *exists);
unsigned char *sh_package_source_bind_derived(sh_package_source_reader *reader,
    const char *family, const char *name, const unsigned char *body, size_t length,
    sh_package_binding_resolver resolver, void *context,
    sh_package_bound_exists exists, void *exists_context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity);

/* Optional localization pass for verified reflected idStrId value leaves.
 * The key span is the exact native byte string, not necessarily UTF-8 or NUL
 * terminated. Return 0 to observe/keep, 1 with a borrowed replacement key, or
 * negative to abort. A collect-only call returns an exact source copy; its
 * observations become trustworthy only after the whole call succeeds. */
typedef int (*sh_package_string_site_resolver)(void *context, const char *slot,
    const char *key, size_t key_length, const char **replacement);
/* Walk explicit own-state leaves through supported reflected declaration and
 * graph envelopes. Ancestor fields/collections use the captured native schema;
 * source inheritance is not flattened or attributed to the child. The caller
 * retains each contributing source component and binds ancestor declarations
 * separately. Empty/NULL literals remain unchanged and emit no string site.
 * Arbitrary idStr/idAtomicString values, metadata and resource references are
 * never rewritten. Custom source grammars require their own proven adapter.
 *
 * This does not change the resource dependency schema, discover map sites,
 * resolve translations, hash string IDs, or publish dictionary rows. Result
 * references counts observed string sites only. Failure returns NULL, reports
 * zero committed changes, and leaves source bytes untouched. */
unsigned char *sh_package_source_bind_strings(sh_package_source_reader *reader,
    const char *family, const char *name, const unsigned char *body, size_t length,
    sh_package_string_site_resolver resolver, void *context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity);

/* Rebind typed map roots and inline entity state through the same native field
 * readers. The resolver maps saved identities to this candidate's names and
 * must be deterministic; source lookup also uses it to infer inherited classes.
 * Only verified resource string sites change. Prose, editorVars and arbitrary
 * native byte strings remain exact, including on a no-op. No live lookup occurs. */
char *sh_package_source_bind_map(sh_package_source_reader *reader,
    const char *json, size_t length, sh_package_binding_resolver resolver, void *context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity);

/* Per-occurrence variant of map binding. site identifies the exact resource
 * value, or the owning inherit field during source-class lookup. Resolver must
 * verify family/name before using that saved occurrence; a different inherited
 * source name is already in the prepared source scope. No process pointers or
 * engine unique IDs are exposed as portable owner keys. */
typedef int (*sh_package_map_binding_resolver)(void *context,
    const sh_package_provenance_site *site, const char *family,
    const char *name, const char **replacement);
char *sh_package_source_bind_map_sites(sh_package_source_reader *reader,
    const char *json, size_t length, sh_package_map_binding_resolver resolver, void *context,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity);
/* Lookup may inspect parents/classes while locating typed sites. Observe runs
 * only for actual final source values, never those metadata reads. It has the
 * same binding return contract; callers retain observations transactionally
 * only when the entire output succeeds. Both callbacks borrow source sites. */
char *sh_package_source_bind_map_sites_observed(sh_package_source_reader *reader,
    const char *json, size_t length, sh_package_map_binding_resolver lookup,
    sh_package_map_binding_resolver observe, void *context, size_t *output_length,
    sh_package_source_binding *result, char *error, size_t capacity);

#endif
