/* Compile source contributions into one native payload per provider path. */
#ifndef SH_PACKAGE_COMPILER_H
#define SH_PACKAGE_COMPILER_H

#include "package_sources.h"
#include "decl_compose.h"
#include "package_owners.h"

/* Return 1 for a SnapMap original, 2 for a campaign-only original, 3 for a
 * built-in product declaration with no game original, 0 if absent,
 * or -1 if unreadable/ambiguous; original bytes are malloc-owned. Campaign-only
 * imports still contribute gameplay even when equal to the original bytes.
 * Never use the modded provider as a baseline. */
typedef int (*sh_package_baseline_reader)(void *context, const char *engine_path,
                                         unsigned char **body, size_t *length);

/* Optional streaming original lookup for opaque resources. Scope has the same
 * meaning as the byte reader (0 absent, 1 SnapMap, 2 campaign, 3 product).
 * Return 1 for a verified answer, 0 to use the ordinary byte reader, or -1 on
 * an operational failure. A nonzero scope requires a strong file identity.
 * The provider must keep the captured original stable for this compilation. */
typedef struct sh_package_original_identity {
    int scope;
    sh_package_file_identity file;
} sh_package_original_identity;
typedef int (*sh_package_baseline_identity_reader)(void *context, const char *engine_path,
    sh_package_original_identity *out, char *error, size_t capacity);

typedef struct sh_package_builtin {
    const char *engine_path;       /* canonical engine path, one per producer */
    const unsigned char *body;
    size_t length;
} sh_package_builtin;

/* Product generators read named inputs through a compiler-supplied view.
 * Read returns1 with malloc-owned bytes,0 absent,-1 failed. The generator has
 * the same result contract:0 means it does not own the requested identity.
 * It must never consult the live provider or mutate authored source files. */
typedef int (*sh_package_producer_reader)(void *context, const char *path,
    unsigned char **body, size_t *length);
typedef int (*sh_package_producer)(void *context, const char *path,
    sh_package_producer_reader read, void *read_context,
    unsigned char **body, size_t *length);

struct sh_package_compilation;

/* Read-only authored inputs for a dependent resource. A custom grammar can
 * project its schema from these bytes without consulting native loaded state.
 * Original uses the installed/product baseline. Contribution selects the outer
 * package owning that source index (or product defaults for the built-in input).
 * Result includes all peer packages and product defaults. No peer is preselected
 * by folder order and no complete native resource is loaded by this service.
 * All bytes and arrays are owned; release them with the function below. */
typedef struct sh_package_resource_inputs {
    sh_decl_source original;
    int original_scope;
    sh_decl_source *contributions;
    size_t *owners; /* SIZE_MAX denotes a product default. */
    size_t count;
} sh_package_resource_inputs;
void sh_package_resource_inputs_free(sh_package_resource_inputs *inputs);
typedef struct sh_package_source_view {
    void *context;
    /* 1 found, 0 absent, -1 unreadable/invalid. Output is empty on failure.
     * index identifies this composer's input only for CONTRIBUTION. Views and
     * callbacks are synchronous; callers must not retain the context pointer. */
    int (*read)(void *context, sh_decl_composition_role role, size_t index,
        const char *engine_path, sh_package_resource_inputs *out,
        char *error, size_t capacity);
} sh_package_source_view;

/* Compiler services are supplied by the product, never package metadata.
 * The baseline reader bypasses all overrides. Built-ins join the original
 * authored contributions in one merge and carry no delivery ownership.
 * Schema metadata must remain stable through the synchronous compilation. */
typedef struct sh_package_compile_environment {
    sh_package_baseline_reader baseline;
    void *baseline_context;
    const sh_package_builtin *builtins;
    size_t builtin_count;
    const sh_decl_dependency_schema *types;
    void *type_context;
    int (*class_derives)(void *context, const char *child, const char *parent);
    /* Non-entity root reader. 1 resolves the reflected edit class, 0 denotes
     * a custom/unsupported grammar, -1 means required metadata is unavailable.
     * The callback shares type_context. No author switches or resource loads.
     * Custom families still accept a single complete replacement or duplicates;
     * different contributions need a format-specific composer. */
    int (*declaration_state)(void *context, const char *declaration_type,
        sh_decl_value_type *state_type);
    sh_package_producer produce;
    void *producer_context;
    /* Stateless identity canonicalizer, with the same owned-string contract
     * as sh_package_engine_path. Retained by the result for runtime lookups. */
    char *(*canonical_path)(const char *path);
    /* Retain previously requested generated identities so removal recompiles
     * them from current inputs. The caller retains this snapshot through build. */
    const struct sh_package_compilation *previous;
    /* Optional registered-reader adapter, sharing type_context. Return 0 when
     * unclaimed, 1 with malloc-owned output, or -1 with a diagnostic. Inputs
     * include built-ins in the same original-relative pass. No native loading
     * or source mutation. It is called only when composition is required. */
    int (*compose_custom)(void *context, const char *declaration_type,
        sh_decl_source baseline, const sh_decl_source *sources, size_t count,
        char **body, size_t *length, char *error, size_t error_capacity,
        sh_decl_conflict *conflict, const sh_package_source_view *source_view);
    sh_package_baseline_identity_reader baseline_identity;
    void *identity_context;
    /* Optional diagnostic output for a proven malformed outer package. Reset
     * to SIZE_MAX on entry. Peer conflicts, missing native metadata and failed
     * originals never select a package to discard. Strict callers omit it. */
    size_t *invalid_package;
} sh_package_compile_environment;

typedef struct sh_compiled_resource {
    const char *engine_path;
    char *type, *name;             /* known declaration identity, otherwise NULL */
    size_t *sources;              /* all input file indices, duplicates retained */
    size_t source_count;
    sh_package_owners owners;         /* changes to the original resource */
    sh_package_owners gameplay_owners; /* excludes editor types/editorVars-only edits */
    int baseline_known, composed; /* original scope: 0 absent, 1 SnapMap, 2 campaign, 3 product */
    int native_original;         /* declaration bytes exactly match an installed game original */
    int restored_original;       /* owns engine_path; no authored source or owners */
    unsigned char *body;          /* owned native text or composed result */
    size_t body_length;
    size_t source;                /* effective opaque/unchanged source file */
    char *cache_path;             /* runtime-owned immutable opaque snapshot */
    int generated;               /* product producer participates in this result */
    char **generated_inputs;      /* canonical input paths, copied by compiler */
    size_t generated_input_count;
    int owns_engine_path;
    sh_package_file *cache_file;  /* retained immutable bytes, independent stream positions */
} sh_compiled_resource;

typedef struct sh_package_policy {
    sh_json_object requirements, strings, hud;
} sh_package_policy;

typedef struct sh_package_compilation {
    /* Input lifetime must exceed compilation lifetime. */
    const sh_package_sources *sources;
    sh_compiled_resource *resources; /* sorted by canonical engine path */
    size_t resource_count;
    size_t duplicate_count, composed_count;
    sh_package_policy policy;
    char *(*canonical_path)(const char *path);
    /* Internal map-provider ownership; never authored or serialized. An
     * overlay owns its copied inventory. Map owners follow local owners. */
    int owns_sources, map_overlay;
    size_t map_owner_begin;
} sh_package_compilation;

/* Compose complete delivery owners selected by the resource graph. The set
 * is internal compiler ownership, never author metadata. Nested components
 * travel with their outer package. Output starts empty and is owned by the
 * caller; failure leaves it empty. Free an earlier result before reusing it. */
int sh_package_compilation_policy(const sh_package_compilation *compilation,
                                    const sh_package_owners *owners, sh_package_policy *out,
                                    char *error, size_t error_capacity);
void sh_package_policy_free(sh_package_policy *policy);

sh_package_compilation *sh_package_compile(const sh_package_sources *sources,
                                            sh_package_baseline_reader baseline,
                                            void *context, char *error,
                                            size_t error_capacity);
sh_package_compilation *sh_package_compile_with(const sh_package_sources *sources,
    const sh_package_compile_environment *environment, char *error, size_t error_capacity);
void sh_package_compilation_free(sh_package_compilation *compilation);
/* Each input has already composed its peer packages. Map resources replace
 * matching local resources; pure product defaults do not hide local edits.
 * The returned provider owns independent copies and retains every authored
 * member. Inputs, their inventories and the filesystem remain unchanged.
 * Overlay providers are terminal views, not inputs to another overlay. */
sh_package_compilation *sh_package_compilation_overlay(
    const sh_package_compilation *local, const sh_package_compilation *map,
    char *error, size_t error_capacity);
/* Retain removed SnapMap or built-in product identities with original bytes so
 * native refresh also rebuilds their existing objects. Neither input changes
 * on failure. Campaign imports and new identities have no SnapMap original. */
int sh_package_compilation_restore_missing(sh_package_compilation *current,
    const sh_package_compilation *previous, sh_package_baseline_reader baseline,
    void *context, char *error, size_t error_capacity);
const sh_compiled_resource *sh_package_compilation_find(const sh_package_compilation *compilation,
                                                        const char *engine_path);
/* Resource availability, independent of package ID and map/local byte
 * differences: 1 readable from this provider, 0 absent, -1 invalid/unreadable.
 * Opaque data is verified through its immutable cache or source handle without
 * allocating a whole resource. This checks one provider path; the caller must
 * resolve the complete dependency set and establish native activation readiness. */
int sh_package_compilation_probe(const sh_package_compilation *compilation,
    const char *engine_path, char *error, size_t error_capacity);
/* Check resolved engine paths, independently of package names and authored
 * byte differences. The callback sees only canonical paths and reports client
 * availability: 1 readable, 0 missing, -1 invalid/unreadable. It must exclude the
 * temporary map provider. This service does not discover gameplay dependencies
 * or establish native activation readiness; callers must supply the closure.
 * Product-generated resources expand to their compiler-recorded input paths.
 * Missing paths select every supplying map delivery owner, preserving duplicate
 * authored packages. A missing path with no delivered source is an error.
 * Initialize the result to zero. Failure clears it; repeated calls replace it. */
typedef int (*sh_package_resource_available)(void *context, const char *engine_path,
    char *error, size_t capacity);
typedef struct sh_package_missing {
    char **paths;                 /* owned, canonical, sorted, unique */
    size_t count, checked;
    sh_package_owners packages;   /* whole map delivery owners to install */
} sh_package_missing;
int sh_package_compilation_missing(const sh_package_compilation *map,
    const char *const *required_paths, size_t count,
    sh_package_resource_available available, void *context,
    sh_package_missing *out, char *error, size_t capacity);
void sh_package_missing_free(sh_package_missing *missing);
typedef enum sh_package_change_kind {
    SH_PACKAGE_RESOURCE_ADDED,
    SH_PACKAGE_RESOURCE_REPLACED,
    SH_PACKAGE_RESOURCE_REMOVED
} sh_package_change_kind;
typedef struct sh_package_change {
    char *path;
    sh_package_change_kind kind;
    /* Compiled declaration identity, independent of its native cooked path.
     * Owned copies survive retirement of the source provider. */
    char *type, *name;
} sh_package_change;
typedef struct sh_package_changes {
    sh_package_change *items;
    size_t count;
} sh_package_changes;
/* Compare effective provider bytes, independent of package identity, ownership,
 * cache filename and authored duplicate count. NULL is an empty provider.
 * Opaque inputs use their captured SHA256/length; mixed memory/file inputs read
 * a verified stream. This does not certify current file availability, discover
 * native consumers or reload anything. Result paths are copied and sorted;
 * they survive freeing either compilation. Initialize out to zero; failure
 * clears it. Publication uses this before entering the native boundary. */
int sh_package_compilation_changes(const sh_package_compilation *before,
    const sh_package_compilation *after, sh_package_changes *out,
    char *error, size_t capacity);
void sh_package_changes_free(sh_package_changes *changes);
/* Check every resource supplied by the compiled map, including generated
 * inputs. A zero-missing result is sufficient to avoid reinstalling this
 * payload, without discovering every dependency the engine may request.
 * Missing entries need not all be used by gameplay; callers may narrow them
 * only with a complete dependency set. Pure product defaults and package
 * documentation are not installation requirements. Same result ownership. */
int sh_package_compilation_payload_missing(const sh_package_compilation *map,
    sh_package_resource_available available, void *context,
    sh_package_missing *out, char *error, size_t capacity);
/* A malloc-owned stream snapshot. An input changed after discovery fails;
 * callers must not silently fall through to a different package or vanilla. */
unsigned char *sh_package_compilation_read(const sh_package_compilation *compilation,
                                            const sh_compiled_resource *resource,
                                            size_t limit, size_t *length,
                                            char *error, size_t error_capacity);

#endif
