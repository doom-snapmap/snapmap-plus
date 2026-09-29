/* Assemble scoped source dependencies for the resource linker. */
#ifndef SH_PACKAGE_SOURCE_LINK_H
#define SH_PACKAGE_SOURCE_LINK_H
#include "package_link_inputs.h"
#include "package_source_graph.h"

typedef struct sh_package_source_link sh_package_source_link;
typedef struct sh_package_audio_program sh_package_audio_program;
typedef struct sh_package_source_memo sh_package_source_memo;
typedef int (*sh_package_source_implicit)(void *context, size_t scope, int observing,
    const sh_package_link_unit *unit,
    int (*visit)(void *, const char *slot, const char *family, const char *name),
    void *visit_context, char *error, size_t capacity);
typedef struct sh_package_source_link_native {
    void *context;
    /* Admit a verified engine-created identity with no physical source. Never
     * infer admission from a currently loaded/modded object or an absent file.
     * 1 admits with stable semantics, 0 is unsupported, -1 is an error. This
     * callback is used only after proven source absence, not a read failure.
     * The linker requires admitted zero-file units to retain their native name. */
    int (*admit)(void *context, size_t scope, const char *family, const char *name,
        unsigned char semantics[32], char *error, size_t capacity);
    /* Bind stable implicit/name-derived state for every file-backed unit into
     * its equivalence identity. Return 1 if this state matches the engine
     * original, 0 if different/uncertified, -1 on an incomplete/failed read.
     * The driver independently checks exact original file bytes; the linker
     * checks explicit dependencies transitively. No callback means no native
     * original certificate. Zero-file units use admit's semantics instead.
     * A product-provided baseline is not an engine-original certificate. */
    int (*semantics)(void *context, size_t scope, const char *family,
        const char *name, unsigned char semantics[32], char *error, size_t capacity);
    /* The current renderer and verified fixed-parameter ABI. Its context is
     * independent of the generic admission/semantics context above. */
    const sh_package_source_shader *shader;
    /* Add exact implicit page-source witnesses after the material's physical
     * declaration is captured. The adapter owns returned metadata through
     * source-link close; each role is unique and the visitor copies its name.
     * These files are streamed for equality, never passed to a decl parser. */
    int (*material_companions)(void *context, size_t scope, const char *material,
        int (*visit)(void *, const char *, const sh_compiled_resource *), void *visit_context,
        char *error, size_t capacity);
    /* Seed materials reached by authored page/table families before reverse
     * root nomination. The visitor captures their exact implicit witnesses. */
    int (*material_seeds)(void *context, size_t scope,
        int (*visit)(void *, const char *), void *visit_context,
        char *error, size_t capacity);
    /* Enumerate proven constructor/consumer resource requests outside the
     * serialized bytes. The unit's physical members have already been captured.
     * Return 1 after the complete walk, 0/-1 on unsupported/failed capture.
     * Each nonempty semantic slot resolves in this scope, never a peer scope.
     * Repeating the same slot/identity is harmless; changing its target is a
     * capture error. Strings are borrowed for the visitor call and copied.
     * observing is nonzero only for reverse-root discovery in the original-only
     * scope. Such units may lack source and are not admitted runtime objects.
     * No runtime load, mutation or assumption of original equivalence is allowed.
     * Consumer publication remains a separate native adapter obligation. */
    sh_package_source_implicit implicit;
    /* Capture delivery ownership after complete discovery and implicit
     * semantics. The adapter owns returned sets until source-link close.
     * Null sets mean no additional owners. This cannot grant admission or
     * substitute for a complete identity certificate. Return 1 on success. */
    int (*owners)(void *context, size_t scope, const char *family, const char *name,
        const sh_package_owners **owners, const sh_package_owners **gameplay,
        char *error, size_t capacity);
    /* Optional retained audio inventory for this exact scope list. Owns raw
     * bank/media source accounting and reverse sound nomination. */
    sh_package_audio_program *audio;
    /* Prepare a single-file unit's effective native fields before dependency
     * discovery. 0 keeps its source; 1 returns malloc-owned equivalent text;
     * -2 denotes invalid authored state, -3 unavailable inherited meaning,
     * -1 an operational failure. A rewrite
     * must preserve native meaning, not introduce policy. Exact source bytes
     * remain the original certificate witness and are never overwritten. */
    int (*prepare)(void *context, sh_package_source_reader *reader,
        const sh_package_link_unit *unit, unsigned char **body, size_t *length,
        char *error, size_t capacity);
    /* Optional discovery recordings retained from earlier captures in this
     * process (sh_package_source_memo_open), borrowed for this capture. */
    sh_package_source_memo *memo;
} sh_package_source_link_native;

/* Enumerate supplied units, complete their catalog permutations and walk the
 * production typed source binders and registered constructor requests until
 * every captured reference resolves.
 * Each identity resolves in its input compilation, then verified originals.
 * Peer scopes never supply missing files. Original reads/absence are captured
 * once per path and shared as immutable bytes; package bytes are not changed.
 * Campaign-only dependencies absent from the authored scope are not silently
 * imported into a bundle. Unsupported/absent readers fail the whole candidate.
 * Font units capture their available 48/64_df/12 glyph forms and exact material
 * request slots. Optional materials use only this scope and SnapMap originals;
 * confirmed absence is retained by omitting that slot from the complete plan.
 * Read failures never count as absence. Native consumer admission remains
 * separate: the production adapter must not enable unbound constructors.
 *
 * This establishes source closure for the registered readers. Complete implicit
 * dependency enumeration and native ABI/alias admission still belong to the
 * native adapter; this is not
 * permission to rename every shader parameter or engine-created identity.
 * Inputs/native metadata must stay stable through close. No native calls,
 * runtime publication, author metadata changes or filesystem writes occur. */
sh_package_source_link *sh_package_source_link_open(
    const sh_package_compilation *const *scopes, size_t count,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_source_link_native *native, char *error, size_t capacity);
typedef struct sh_package_source_root_plan {
    /* One byte per scope. Discover baseline SnapMap palette roots affected by
     * this scope's supplied units; never combine independent source scopes. */
    const unsigned char *discover;
    size_t baseline_scope; /* Existing original-only comparison scope. */
    /* Recorded bindings seed their exact original names in their recorded
     * context indices. This reconstructs derived roots without promoting
     * unused authoring inventory into a document's selected occurrences. */
    const sh_package_provenance *recorded;
    /* The typed values a legacy document actually names. When set, discovery
     * walks these stock roots instead of the SnapMap palette, so a discovering
     * scope nominates only the ones it changes that the document uses. */
    const sh_package_references *candidates;
} sh_package_source_root_plan;
sh_package_source_link *sh_package_source_link_open_roots(
    const sh_package_compilation *const *scopes, size_t count,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_source_link_native *native, const sh_package_source_root_plan *roots,
    char *error, size_t capacity);
/* Where a failed closure stopped and why. Each discovered unit remembers the
 * unit whose reader first requested it, so a failure carries its causal chain
 * back to the seed that reached it. Only content an input scope supplied can
 * make that scope responsible:
 * SOURCE: the text of the scope's own supplied unit does not parse in the
 *   native grammar (a reader without an adapter for it is COVERAGE).
 * COVERAGE: the scope's supplied content (or a stock root nominated because
 *   of it) reaches a unit the product cannot link: missing reader/admission
 *   coverage, or a dependency SnapMap does not ship and the scope does not
 *   supply. The scope's files are not proven corrupt.
 * PRODUCT: only product built-ins and stock units reach the failing unit.
 * ENVIRONMENT: I/O, allocation, changed native state or setup.
 * scope is the responsible input scope for SOURCE/COVERAGE, else SIZE_MAX. */
typedef enum sh_package_source_failure_kind {
    SH_PACKAGE_SOURCE_FAILURE_NONE,
    SH_PACKAGE_SOURCE_FAILURE_ENVIRONMENT,
    SH_PACKAGE_SOURCE_FAILURE_PRODUCT,
    SH_PACKAGE_SOURCE_FAILURE_COVERAGE,
    SH_PACKAGE_SOURCE_FAILURE_SOURCE
} sh_package_source_failure_kind;
typedef struct sh_package_source_failure {
    sh_package_source_failure_kind kind;
    size_t scope;
    char family[64], name[256];
    char chain[512]; /* "family name <- requester <- ... <- seed". */
} sh_package_source_failure;
sh_package_source_link *sh_package_source_link_open_attributed(
    const sh_package_compilation *const *scopes, size_t count,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original,
    void *original_context, sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_source_link_native *native, const sh_package_source_root_plan *roots,
    sh_package_source_failure *failure, char *error, size_t capacity);
void sh_package_source_link_close(sh_package_source_link *source);
/* Discovery recordings that outlive the capture that made them, for later
 * captures in this process. A recording keeps a unit's preparation output and
 * ordered reference and request sites, with every file and scoped path its
 * reader read and how each resolved: supplied by the scope, or the capture's
 * original with its status, and its content identity. A later capture replays
 * it only after resolving every one of them afresh to the same content;
 * replay still resolves each reference in its own scope and reruns file
 * selection, material companions and constructor requests. The memo is keyed
 * by what readers consult outside those paths, read afresh by every capture:
 * the catalog rows (sh_resource_catalog_digest), the native managers and
 * reflection (sh_package_source_reader_native_digest), the product's script
 * capture and the preparation callback. A different key drops every
 * recording. Readers of the native shader ABI (renderprog, renderprogflag)
 * are never recorded. Portable data only (strings, identities, byte copies);
 * never persisted. One capture borrows the memo at a time; a capture that
 * finds it busy records only for itself. */
sh_package_source_memo *sh_package_source_memo_open(void);
void sh_package_source_memo_close(sh_package_source_memo *memo);
/* Reverse discovery stops at stock sources beyond the registered readers
 * instead of refusing every package. scope SIZE_MAX names the stock unit whose
 * reader stopped; any other scope changed a stock root whose closure crosses
 * such a unit, so no derived variant was exposed for it. Unreadable originals
 * and allocation failures still fail the candidate. Borrowed until close. */
typedef struct sh_package_reverse_gap {
    size_t scope;
    const char *family, *name, *reason;
} sh_package_reverse_gap;
size_t sh_package_source_link_reverse_gaps(const sh_package_source_link *source,
    const sh_package_reverse_gap **gaps);
/* Borrowed, captured source units, sorted by scope/family/name. */
const sh_package_link_inputs *sh_package_source_link_inputs(const sh_package_source_link *source);
/* Write "readers N ms, seeds N ms, ..." for the capture that opened source;
 * returns out. A diagnostic only. */
const char *sh_package_source_link_phases(const sh_package_source_link *source, char *out, size_t capacity);
/* Diagnostic counts of the capture, as one line: units discovered and
 * replayed from another scope's discovery, file binds, original captures. */
const char *sh_package_source_link_counters(const sh_package_source_link *source, char *out, size_t capacity);
/* The payload identity of one file of source unit index, as
 * sh_package_compilation_identity computes it for that unit's compilation;
 * an unsupplied original's is computed once for every unit that binds it. */
int sh_package_source_link_identity(sh_package_source_link *source, size_t index,
    const sh_compiled_resource *file, sh_package_file_identity *out, char *error, size_t capacity);
/* Tests and diagnostics: discovery replay of unchanged units is on by
 * default; 0 binds every unit afresh (the same result, more slowly). */
void sh_package_source_link_replay(int enabled);
/* Diagnostics: 1 when input unit index was discovered by replay. */
int sh_package_source_link_unit_replayed(const sh_package_source_link *source, size_t index);
/* One byte per input unit: its complete captured dependency closure has no
 * gameplay contribution. Editor collection composition uses this eligibility;
 * an editor resource reaching private gameplay must follow that asset scope.
 * Zero-file originals and product-only sources contribute no gameplay owners.
 * This is NOT native-original certification or a map-embedding decision: native
 * implicit state and actual map usage remain separate. Borrowed until close. */
const unsigned char *sh_package_source_link_shared(const sh_package_source_link *source);
/* Assignment-free binding view for shared collection composition. These use
 * exactly the frozen source snapshot and unit indices returned by inputs().
 * bind_unit may inspect a derived body, but its inherited class/schema lookup
 * remains in that unit's authored scope. Cross-scope entries must retain their
 * captured target indices when subsequently replayed; their spelling is not a
 * request to resolve again in the destination scope. Owned output bytes. */
int sh_package_source_link_identify(sh_package_source_link *source,
    sh_package_link_classes *out, char *error, size_t capacity);
int sh_package_source_link_resolve_unit(sh_package_source_link *source, size_t unit,
    const char *family, const char *name, size_t *target, char *error, size_t capacity);
/* The frozen unit with this family and name in one input scope, if any. */
int sh_package_source_link_find_unit(sh_package_source_link *source, size_t scope,
    const char *family, const char *name, size_t *unit);
unsigned char *sh_package_source_link_bind_unit(sh_package_source_link *source, size_t unit,
    const unsigned char *body, size_t length, sh_package_binding_resolver resolver,
    void *context, size_t *output_length, char *error, size_t capacity);
/* Typed idStrId sites use the same frozen source/inheritance scope. This pass
 * neither treats prose as a key nor changes resource dependency collection. */
unsigned char *sh_package_source_link_bind_strings(sh_package_source_link *source, size_t unit,
    const unsigned char *body, size_t length, sh_package_string_site_resolver resolver,
    void *context, size_t *output_length, char *error, size_t capacity);
/* Registration-key sites of one captured unit, in its own scope; see
 * sh_package_source_bind_keys for the status contract. */
int sh_package_source_link_bind_keys(sh_package_source_link *source, size_t unit,
    const unsigned char *body, size_t length, sh_package_key_site_resolver resolver, void *context,
    unsigned char **output, size_t *output_length, char *error, size_t capacity);
int sh_package_source_link_key_space(sh_package_source_link *source, struct sh_package_key_space *space);
/* 1 when the stock player the engine spawns for co-op (the rules entity's
 * playerSpawnDef, default player/mp) names this animation web in
 * idPlayer::handsAnimWeb or _D1: the one global web every weapon's subweb is
 * resolved in. */
int sh_package_source_link_player_hands(const sh_package_source_link *source, const char *name);
/* The same stock contract read through a baseline reader alone: the names of
 * the hands webs (malloc-owned, possibly none). 0 when a stock entity it needs
 * is unreadable. */
int sh_package_player_hands_webs(sh_package_baseline_reader original, void *context,
    char ***names, size_t *count);
/* Physical members of multi-file units use their captured role. In particular,
 * shader source, cooked records and stage modules are different formats. */
unsigned char *sh_package_source_link_bind_file(sh_package_source_link *source, size_t unit,
    const sh_compiled_resource *file, const unsigned char *body, size_t length,
    sh_package_binding_resolver resolver, void *context, size_t *output_length,
    char *error, size_t capacity);
/* The reference sites that discovery bound in one captured file of a unit,
 * in bind order, as (slot, target unit) pairs: exactly the sites and frozen
 * resolutions bind_file reports for that file, since discovery ran the same
 * reader over the same bytes. 1 with the sites (possibly none), 0 when this
 * link did not discover that file. Valid until the link closes. */
int sh_package_source_link_sites(const sh_package_source_link *source, size_t unit,
    const sh_compiled_resource *file, const sh_package_link_edge **sites, size_t *count);
/* Enumerate a complete captured shader unit under a prospective root identity.
 * PERMUTATION entries are native renderprog identities, not provider paths.
 * The allocator must reserve/check every such ID along with the root before
 * publication. Other entries map original member paths to linked paths. */
int sh_package_source_link_shader_paths(sh_package_source_link *source, size_t unit,
    const char *linked_name, sh_package_shader_path_visitor visitor, void *context,
    char *error, size_t capacity);
unsigned char *sh_package_source_link_bind_derived(sh_package_source_link *source, size_t unit,
    const unsigned char *body, size_t length, sh_package_binding_resolver resolver,
    void *context, sh_package_bound_exists exists, void *exists_context,
    size_t *output_length, char *error, size_t capacity);
/* resolve/bind use the retained scoped readers and frozen original snapshot.
 * The supplied native assignment establishes valid aliases, including implicit
 * ABI. Its registry callback checks native ID/name occupation as aliases are
 * allocated; that callback retains its own context through this call.
 * Output is independent of this builder after a successful link. */
int sh_package_source_link_compile(sh_package_source_link *source,
    const sh_package_sources *inventory, const sh_package_link_adapter *native,
    const sh_package_policy *policy, char *(*canonical_path)(const char *),
    sh_package_link_output *out, char *error, size_t capacity);
#endif
