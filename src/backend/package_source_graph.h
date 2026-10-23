/* Dependency inspection of a retained candidate's declaration bytes. */
#ifndef SH_PACKAGE_SOURCE_GRAPH_H
#define SH_PACKAGE_SOURCE_GRAPH_H

#include "package_usage.h"
#include "decl_native_registry.h"
#include "decl_native_schema.h"
#include "package_provenance.h"
#include "package_shader.h"
#include "audio_originals.h"
#include "native_class_registry.h"

typedef struct sh_package_source_graph_report {
    size_t declarations, references, gaps;
    char first_gap[1024];
    /* Reader gaps inside an unchanged SnapMap original declaration. Such a
     * declaration and its whole closure are natively available: any changed
     * dependency binds a private or published unit instead, which is walked.
     * These do not make the closure incomplete. */
    size_t native_gaps;
    /* The effective entity pool settings construction was judged under
     * (sh_native_pool_settings_read, once per walk): 0 none were needed, 1
     * read (values below), -1 unavailable. A result kept for later use holds
     * only while the process still reports these values. */
    int pool_settings_state;
    sh_native_pool_settings pool_settings;
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
     * Return 1 with verified kind (0..18) and native index (0..32767), 2 after
     * retaining a native preparation request (the caller retries after it; the
     * flag callback below answers 2 alike), 0 for missing/incompatible
     * coverage, -1 on failed read. Same-name occupancy or
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
     * the engine. Return 1 only after retaining a request, 0 when unsupported,
     * or -1 on a failed read. The caller retries the same immutable scope after
     * preparation; a queued request is not a usable compiled program. */
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
/* Those families, and a map's light-probe image in the file family. */
int sh_package_source_leaf(const char *family, const char *name);
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
/* As sh_package_source_reader_open, reading base's native reflection records
 * (sh_decl_native_schema_open_shared) when registry's read, context and scripts
 * and reflection are base's, instead of keeping another copy. Readers sharing
 * records close in any order and must not be used concurrently. NULL base
 * opens a reader of its own. */
sh_package_source_reader *sh_package_source_reader_open_shared(const sh_package_source_reader *base,
    const sh_package_compilation *compiled, const sh_resource_catalog *catalog,
    sh_package_baseline_reader original, void *original_context, sh_decl_registry_source registry,
    uintptr_t reflection, char *error, size_t capacity);
void sh_package_source_reader_close(sh_package_source_reader *reader);
/* Optional only for scopes that never reach a renderprog. Once configured,
 * renderer and ABI contract are copied; caller-owned callback state stays live. */
int sh_package_source_reader_shader(sh_package_source_reader *reader,
    const sh_package_source_shader *shader);
int sh_package_source_reader_audio(sh_package_source_reader *reader,
    const sh_package_source_audio *audio);
/* Optional observer of every scoped path a reader reads or checks while it
 * binds, prepares or requests (the scope's own file, else the captured
 * original), with whether the scope supplied it. Graph inspection
 * (sh_package_source_graph*) is outside this contract. NULL removes it. */
typedef void (*sh_package_source_lookup)(void *context, const char *path, int supplied);
void sh_package_source_reader_lookups(sh_package_source_reader *reader,
    sh_package_source_lookup lookup, void *context);
/* Optional: what the reader's original reader returns for path (1 SnapMap, 2 campaign, 3 product, 0 absent,
 * -1 failed) without reading its bytes, given the reader's original_context. It must answer exactly as that
 * reader does. A binding that needs only an original's status then asks this instead of copying the original. */
typedef int (*sh_package_source_original_status)(void *context, const char *path);
void sh_package_source_reader_original_status(sh_package_source_reader *reader,
    sh_package_source_original_status status);
/* SHA-256 of every native manager and reflected fact this reader's typed
 * binding can consult, read afresh (sh_decl_native_registry_digest,
 * sh_decl_native_schema_digest). Readers with equal digests bind the same
 * bytes identically. 1 with the digest. */
int sh_package_source_reader_native_digest(sh_package_source_reader *reader, unsigned char out[32]);
/* 1 when the reader's last shader open or bind failed only because a native
 * parameter or flag check retained a preparation request. */
int sh_package_source_reader_preparing(const sh_package_source_reader *reader);
typedef enum sh_package_source_shader_status {
    SH_PACKAGE_SOURCE_SHADER_ERROR = 0,
    SH_PACKAGE_SOURCE_SHADER_READY = 1,
    SH_PACKAGE_SOURCE_SHADER_PREPARING = 2
} sh_package_source_shader_status;
/* Decode the selected renderer's cooked program with scoped parameter kinds.
 * Validate every retained fixed parameter ABI and the declaration checksum.
 * All returned bytes/program arrays are owned by the caller only on READY.
 * PREPARING means a missing cooked program has a retained native request, not
 * a coverage gap. Outputs are empty until a subsequent READY result. */
sh_package_source_shader_status sh_package_source_shader_open(sh_package_source_reader *reader, const char *name,
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
/* Material construction resolves the two native transparency String parameters
 * as atlas names and may fall back to ordinary images of the same name. Capture
 * both requests in this source scope without rewriting the authored String or
 * coupling the two managers' generated filenames. Observers retain absent edges
 * for reverse discovery; serving captures only supplied or SnapMap sources. */
int sh_package_source_material_requests(sh_package_source_reader *reader,
    sh_decl_source source, int observing,
    int (*visit)(void *, const char *, const char *, const char *), void *context,
    char *error, size_t capacity);

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
    /* Binding stopped on allocation or an unreadable parent source, never
     * evidence about the declaration: the caller keeps its prior generation
     * instead of judging content. */
    int environment;
    /* Bound, but the declaration's native semantics are undecided: its entity
     * state selection is remaining product coverage, so every statement was
     * bound and the text is served as authored. Complete dependency capture
     * proves no effective native state, equivalence or vanilla classification. */
    int coverage;
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

/* Registration-key sites (package_keys.h): every reflected spawn-type value
 * of one declaration, walked like localized string sites. The resolver sees
 * the whole flag list and returns 1 with a replacement, 0 to keep it, or -1.
 * Returns 1 with a malloc-owned copy (result->changed counts rewritten sites),
 * 0 for a family with no reflected state reader, -1 on failure; source bytes
 * are never modified. */
typedef int (*sh_package_key_site_resolver)(void *context, const char *slot,
    const char *value, size_t length, const char **replacement);
int sh_package_source_bind_keys(sh_package_source_reader *reader,
    const char *family, const char *name, const unsigned char *body, size_t length,
    sh_package_key_site_resolver resolver, void *context, unsigned char **output,
    size_t *output_length, sh_package_source_binding *result, char *error, size_t capacity);
/* The key enum as this reader's reflection registers it (package_keys.h). */
struct sh_package_key_space;
int sh_package_source_key_space(sh_package_source_reader *reader, struct sh_package_key_space *space);
/* Materialize selected inherited edit fields in this source's own envelope.
 * Native schema membership filters unrelated entity classes. Parent reads use
 * the reader's immutable scope; metadata and unrelated fields remain authored.
 * Returns 1 with owned output, 0 if no inherited selected field exists, -2 for
 * unavailable source semantics, or -1 on an operational failure. It does not
 * invent constructor defaults or mutate input files. */
int sh_package_source_entity_fields(sh_package_source_reader *reader, const char *name,
    const unsigned char *body, size_t length, const char *const *fields, size_t field_count,
    unsigned char **output, size_t *output_length, char *error, size_t capacity);
/* The same, also reporting the resolved state (malloc-owned): "" for the
 * first stateful layer, the inherited first statement's name in update mode,
 * NULL when the fields were not prepared. */
int sh_package_source_entity_fields_state(sh_package_source_reader *reader, const char *name,
    const unsigned char *body, size_t length, const char *const *fields, size_t field_count,
    unsigned char **output, size_t *output_length, char **state, char *error, size_t capacity);

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
/* Registration-key groups in map-authored values (package_keys.h): groups is
 * the serving generation's table that new sets join, other the table beside
 * it, taken the codes serving keys hold; saved_keys lists the keys a saved
 * map recorded at one value site ("<site>#<key>"), for the binding context. */
typedef int (*sh_package_saved_keys_fn)(void *context, const sh_package_provenance_site *site,
    uint32_t *keys, size_t capacity);
struct sh_package_key_groups;
struct sh_package_key_set;
void sh_package_source_reader_key_codes(sh_package_source_reader *reader, struct sh_package_key_groups *groups,
    const struct sh_package_key_groups *other, const struct sh_package_key_set *taken, sh_package_saved_keys_fn saved_keys);
/* Actual-reader projection of inherited members (R-249). A placed copy whose
 * class differs from its parent's reads, under its own class, members a
 * package layer of its parent chain authored that the layer's class does not
 * reflect and its deriving classes type differently; publication leaves those
 * spellings unbound. The resolver binds one such member for the copy at owner
 * in the source context of the copy's inherit occurrence, which must supply
 * the authoring layer (layer: its served name, NULL when unknown). op 0: 1 with the
 * bound name, 0 when that context supplies none (the spelling keeps its global
 * meaning), 2 when the saved map records a value at the member's own site (an
 * authored override), negative on failure or an ambiguous context. op 1: as op
 * 0, and records the projection as a use at "<member>#projected" (explicit
 * projection provenance: the selected supplier, never the member's site).
 * op 2: 1 with the name the saved map recorded as that member's projection,
 * 0 when it recorded none. Mode 1 (capture) leaves bytes unchanged; an own
 * member equal to its recorded projection is bound unobserved and recorded
 * again as a projection. Mode 2 (a document the engine loads) removes such
 * members, binds, then inserts and records each projection the copy's own
 * state does not set. Any other own value is authored. Mode 0 disables both. */
typedef int (*sh_package_map_projection_resolver)(void *context, int op, const char *owner, const char *inherit,
    const char *layer, const char *member, const char *family, const char *name, const char **replacement);
void sh_package_source_reader_project_mode(sh_package_source_reader *reader, int mode);
void sh_package_source_reader_projection(sh_package_source_reader *reader,
    sh_package_map_projection_resolver resolver, void *context);
/* R-255: op 3 binds like op 0 without the saved authored-site check, for a
 * member the copy's own state may set. In mode 2 a copy whose class reads
 * bound members names instead its parent's reader-bound variant
 * (package_variants.h), stating every such member, when the sink accepts its
 * text (1: served for this document, 0: insert the projections as above,
 * -1 failure). Each projection is recorded; a copy stating a member keeps it.
 * Without a sink only an already served variant is named. dependencies are
 * the canonical paths the variant's text names (its parent, then each bound
 * member's declaration). Mode 1 (capture) reads a document naming served
 * variants in its parent form: each member a copy inherits from its variant
 * is stated (sh_package_variant_fold) and bound as a projection, so the
 * capture's output and provenance never name a variant. */
typedef int (*sh_package_variant_sink)(void *context, const char *name, const unsigned char *text, size_t length,
    const char *const *dependencies, size_t dependency_count);
void sh_package_source_reader_variant_sink(sh_package_source_reader *reader, sh_package_variant_sink sink, void *context);
/* Observe every gap the reader counts, with the text first_gap would hold
 * ("<type>/<name> <path> (<owner>): <reason>"). It changes no result. */
typedef void (*sh_package_gap_sink)(void *context, const char *gap);
void sh_package_source_reader_gap_sink(sh_package_source_reader *reader, sh_package_gap_sink sink, void *context);
char *sh_package_source_bind_map_sites_observed(sh_package_source_reader *reader,
    const char *json, size_t length, sh_package_map_binding_resolver lookup,
    sh_package_map_binding_resolver observe, void *context, size_t *output_length,
    sh_package_source_binding *result, char *error, size_t capacity);

/* Map-local play declarations (package_native_inline.h). SnapMap play registers
 * a copy of each placed entity's declaration under "%u_%s/%s_%u" before any
 * entity spawns, and an entity's typed state resolves its declaration names
 * when it spawns, so such a name in that state names that occurrence's copy.
 * With these readers, a map binding resolves an entityDef state name that no
 * declaration source supplies to the one placed entity whose copy it names,
 * rewritten for that entity's bound inherit. It needs no source and records no
 * resource use: the observe callback receives SH_PACKAGE_MAP_LOCAL_FAMILY with
 * the bound name at the referencing site. Without readers, or for any other
 * name, binding is unchanged: a name no source supplies stays unresolved. */
#define SH_PACKAGE_MAP_LOCAL_FAMILY "map-local entitydef"
struct sh_native_inline_generated;
struct sh_native_inline_generated_set;
typedef struct sh_package_map_local_names {
    int (*read)(const char *json, size_t length, struct sh_native_inline_generated_set *out);
    char *(*name)(const struct sh_native_inline_generated *row, const char *inherit);
    void (*release)(struct sh_native_inline_generated_set *set);
} sh_package_map_local_names;
void sh_package_source_reader_map_local(sh_package_source_reader *reader, const sh_package_map_local_names *names);

#endif
