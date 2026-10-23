/* Link resolved source scopes into one provider without changing their bundles. */
#ifndef SH_PACKAGE_LINKER_H
#define SH_PACKAGE_LINKER_H

#include "package_compiler.h"
#include "package_link_graph.h"
#include "package_provenance.h"

/* One native identity may consume several physical files (permutations, for
 * example). Their order has no semantic meaning. All file indices and owners
 * must use the original inventory supplied to link; component compilations
 * retain these indices. A zero-file unit denotes an admitted native identity,
 * not a missing dependency. Admission and complete reader coverage remain the
 * native adapter's responsibility. */
typedef struct sh_package_link_witness {
    const char *role;
    const sh_compiled_resource *resource;
} sh_package_link_witness;

typedef struct sh_package_link_file_identity {
    const char *family, *name;
} sh_package_link_file_identity;

typedef struct sh_package_link_unit {
    const char *family, *name;
    const sh_package_compilation *compilation;
    const sh_compiled_resource *const *files;
    size_t file_count;
    unsigned char semantics[32];
    /* Exact original bytes AND native implicit semantics, before dependency
     * propagation. Never derive this from resource availability alone. 2 marks
     * campaign-only bytes a scope supplied: the unit keeps its native name while
     * its dependencies keep theirs, but never makes a dependent original. */
    int original_binding;
    /* Exact, potentially very large implicit source inputs. Sorted role/path
     * and streamed bytes participate in equivalence. These are not manager
     * identities, relocatable member files, or new roots. The family adapter
     * must project/retain them and their dependency ownership separately.
     * Roles are unique per unit; input order has no meaning. */
    const sh_package_link_witness *witnesses;
    size_t witness_count;
    /* Optional file_count entries in the input files order. An empty entry
     * inherits the unit identity; a nonempty entry describes a native identity
     * coupled to this unit (for example, a movie's image atlas). At least one
     * member must use the unit identity. These authored identities participate
     * in exact equivalence. They are not independently resolved graph roots.
     * Repeated identities permit multiple physical permutations. */
    const sh_package_link_file_identity *file_identities;
    /* Verified constructor/consumer dependencies not serialized in any member
     * file. Targets are indices in this complete input unit snapshot, already
     * resolved in the owner's authored context. Slots are unit-local semantic
     * roles; they never collide with physical-file reference slots. Order and
     * exact repeated entries have no meaning. A slot cannot name two targets.
     * These edges participate in equivalence, original certification and
     * lifetime retention. Runtime adapters still own native consumer binding. */
    const sh_package_link_edge *implicit;
    size_t implicit_count;
    /* Consumer dependencies that bind only a changed target: the state-machine
     * declarations a behavior's layer configuration selects, which every AI
     * of that behavior builds from (fsm_web.h). Each is exactly an implicit
     * binding while its target is not certified original over the complete
     * graph (its own bytes and everything it reaches, these edges included),
     * and absent otherwise, so an unchanged target leaves its dependent's
     * identity and name as they were without it. Implicit slot rules; a slot
     * never appears in both lists. */
    const sh_package_link_edge *conditional;
    size_t conditional_count;
    /* Ownership of implicit source records whose exact behavior is already
     * represented by semantics/edges. These sets affect delivery and shared
     * editor eligibility, never equality or alias assignment. They do not add
     * foreign file indices to this declaration's physical source list. Borrowed
     * through linking; the output receives independent sets. */
    const sh_package_owners *implicit_owners, *implicit_gameplay_owners;
    /* A unit every consumer reaches by its one native name (the player hands
     * web). Each scope binds that same name, so neither its per-scope content
     * nor its changes enter a dependent's identity or original certification;
     * its own bytes and edges are identified and certified as usual. */
    int stable_name;
    /* This compilation intentionally binds the shared original native slot.
     * Different from a private copy of the same bytes: dependents must retain
     * that distinction. The assignment adapter must keep this unit's name.
     * Internal native policy, never package metadata. */
    int fixed_name;
    /* Every consumer reaches this unit only through derived publication
     * (sh_package_link_adapter.derive): its owners' consumers derive its path
     * from the owner's name, as the engine derives a module's members from the
     * module's path. A class all of whose units are carried takes no name or
     * path of its own: an original keeps its native one; a changed one is
     * published only where its owners derive it. */
    int carried;
    /* Identity provenance (R-397): the packages, beyond its files' owners, whose content this unit's own content or
     * binding was built from (its scope's package, the members of a composition it took part in). Optional;
     * borrowed through linking. The linker closes it over the unit's identity edges (sh_package_link_output.provenance);
     * it never enters equality, names or certification. */
    const sh_package_owners *provenance;
} sh_package_link_unit;

typedef int (*sh_package_link_reference)(void *context, const char *slot,
    const char *family, const char *name, const char **replacement);

enum sh_package_link_registry_result {
    SH_PACKAGE_LINK_REGISTRY_ERROR = -1,
    SH_PACKAGE_LINK_REGISTRY_COLLISION = 0,
    SH_PACKAGE_LINK_REGISTRY_AVAILABLE = 1,
    SH_PACKAGE_LINK_REGISTRY_REPLACE_SHARED = 2,
    /* Same name, no conflicting numeric identity or original reservation.
     * Only the linker's retained exact binding proof can admit reuse. */
    SH_PACKAGE_LINK_REGISTRY_OCCUPIED = 3
};

typedef struct sh_package_link_registry {
    void *context;
    /* Resolve the logical family to its actual native registry type spelling,
     * compute the verified native ID for this name, and check every
     * baseline/current-live registry that can still be reached by consumers.
     * Return AVAILABLE when free or certified to name this exact binding,
     * COLLISION for an occupied name/ID, ERROR for incomplete verification.
     * OCCUPIED reports only a same-name occupant after every numeric collision
     * and baseline reservation check. The linker must prove its retained exact
     * binding before treating that result as available.
     * REPLACE_SHARED separately admits a changed binding at the same original
     * name of a verified shared native slot. Only its contribution adapter's
     * publication/rollback contract may authorize that result; it is neither
     * equality nor an exemption from other name/ID collision checks. It is
     * invalid for an alias, zero-file identity, or certified original binding.
     * No author metadata grants this decision. Write a nonzero native_id for
     * every non-error result. The binding digest is a lookup
     * aid; reuse needs retained exact binding evidence, not a digest alone.
     * original_binding permits a verified original already in the baseline.
     * This is a read-only check over a stable snapshot, never registration. */
    int (*check)(void *context, const char *family, const char *name,
        const unsigned char binding[32], int original_binding,
        uint32_t *native_id, char *error, size_t capacity);
} sh_package_link_registry;

typedef struct sh_package_link_adapter {
    void *context;
    /* Resolve only in this unit's authored scope. Return 1 with a unit index,
     * or 0 with a diagnostic. No lookup against the live modded provider. */
    int (*resolve)(void *context, size_t unit, const char *family, const char *name,
        size_t *target, char *error, size_t capacity);
    /* Inspect/rebind exactly this file. Return malloc-owned output, including
     * an exact copy for a no-op, or NULL on incomplete/failed inspection. Call
     * reference for every verified binding with stable file-local slots. */
    unsigned char *(*bind)(void *context, size_t unit, const sh_compiled_resource *file,
        const unsigned char *body, size_t length, sh_package_link_reference reference,
        void *reference_context, size_t *output_length, char *error, size_t capacity);
    /* Assign the verified native name and physical paths for an equivalence
     * class. Files are sorted by original path. All returned strings/arrays
     * are malloc-owned, including on failure. Native identities with no files
     * must retain their name. A format adapter must prove native relocation,
     * including any name-derived paths/ABI, before assigning an alias here.
     * The identity argument is a deterministic naming token. It normally equals
     * the binding digest, but changes on collision retries; never use it as
     * provenance or proof of bound-object equality. All generated candidates
     * must respect the native consumer's name/path capacity on every attempt.
     * original_binding certifies the entire resolved graph, conservatively
     * across all equivalent suppliers, not just this representative's bytes. */
    int (*assign)(void *context, size_t unit, const unsigned char identity[32], int original_binding,
        const sh_compiled_resource *const *files, size_t count,
        char **name, char ***paths, char *error, size_t capacity);
    /* Required by native publication. Pure/offline adapters may omit this;
     * those results certify string/path uniqueness only, not native ID safety.
     * Own context lets source-scope wrappers forward the registry unchanged. */
    const sh_package_link_registry *registry;
    /* Required only for coupled member identities. Given the assigned primary
     * name and physical paths, return count malloc-owned native names, in the
     * same sorted file order. The array and any partial strings are owned by
     * the linker even on failure. Primary members must retain name; repeated
     * authored member identities must receive the same assigned name. All
     * names/IDs are checked and reserved atomically with the unit's assignment;
     * a collision retries the entire group. This callback never registers or
     * publishes native objects. Families come from file_identities, not here. */
    int (*assign_members)(void *context, size_t unit, const char *name,
        const sh_compiled_resource *const *files, const char *const *paths, size_t count,
        char ***names, char *error, size_t capacity);
    /* Optional; NULL captures every unit on every pass. */
    struct sh_package_link_reuse *reuse;
    /* Process-lifetime binding records and native alias reservations. The
     * caller serializes access. Successful links reserve identities even if
     * later activation fails: native defaulted objects may still exist. */
    struct sh_package_link_history *history;
    /* Optional: 1 when bind is a deterministic function of the unit's bytes
     * and a resolution frozen for the whole link. Projection then need not
     * replay a unit whose files are all held in memory: its bytes cannot
     * change, and with every target keeping its name it projects to itself.
     * NULL replays every unit. */
    int (*stable)(void *context, size_t unit);
    /* Optional: the reference sites bind would report for one file of a
     * unit, in order, as (slot, target unit) pairs, when the caller already
     * bound those bytes with the same reader and resolution. A capture then
     * reads and identifies the file without binding it again; projection
     * still replays with bind. 1 supplied, 0 bind instead, -1 failure. */
    int (*sites)(void *context, size_t unit, const sh_compiled_resource *file,
        const sh_package_link_edge **sites, size_t *count);
    /* Optional: the payload identity of one file (or witness) of a unit,
     * exactly as sh_package_compilation_identity computes it for the unit's
     * compilation, from a cache the caller keeps for resources many units
     * share. NULL computes it for every captured unit. */
    int (*identity)(void *context, size_t unit, const sh_compiled_resource *file,
        sh_package_file_identity *out, char *error, size_t capacity);
    /* Optional developer diagnostics for a refused native identity: describe
     * writes the caller's own metadata for one unit (scope, owner, composition
     * state) into out; note receives each bounded report line naming both
     * claimants, their classes, certification, names, payloads and bound
     * dependencies. Neither changes what the link decides. */
    void (*describe)(void *context, size_t unit, char *out, size_t capacity);
    void (*note)(void *context, const char *line);
    /* Optional: this link's identity pass is the reuse cache's last. Its retained order and node shapes, which only
     * a later identity pass would read, are freed once the pass has its classes (the captures it restored from stay:
     * the pass's work borrows them). */
    int release_order;
    /* Optional derived publication of an implicit edge's target. Given the
     * owner unit's assigned name and one of its implicit slots, return 1 with
     * the physical path and native name (in the target's family) its consumers
     * derive from that name, 0 to publish nothing for the edge, -1 on failure;
     * 2 is 1 where a certified installed target may be served as an
     * installed-original alias (sh_compiled_resource.installed_alias) rather
     * than a copy of its bytes (never for a declaration, which registers its
     * derived name from its own text).
     * Called while the owner is assigned, so derived paths and identities are
     * checked and reserved with its name and a collision retries the whole
     * group; never for a certified original owner. The target's bound bytes are
     * published there, with its dependencies. Targets must have one file.
     * Strings are malloc-owned by the linker, including on failure. */
    int (*derive)(void *context, size_t unit, const char *name, const char *slot, size_t target,
        char **path, char **native_name, char *error, size_t capacity);
} sh_package_link_adapter;
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
/* Fail sh_package_link right after its projection (tests). */
void sh_package_link_test_after_projection_refusal(int enabled);
#endif

/* In memory only: each binding identity the process issued a private name for,
 * the record of the component identities it was derived from (payload file
 * identities, bound dependency identities, native options and name contract),
 * and the names and IDs claimed for it. Nothing is written to disk. Repeated
 * equal compilations add neither records nor reservations; a repeated identity
 * with another record is refused. Close only after all native identities in
 * this registry cease to exist (normally process teardown). */
typedef struct sh_package_link_history sh_package_link_history;
sh_package_link_history *sh_package_link_history_open(void);
void sh_package_link_history_close(sh_package_link_history *history);
size_t sh_package_link_history_count(const sh_package_link_history *history);
/* R-348: a compile that may be repeated (a reuse attempt) marks the history first; rolling back to the mark
 * releases what links committed since (a link appends nodes and claims after the ones it kept). 0 when the mark is
 * not a prefix of the history. */
typedef struct sh_package_link_history_mark { size_t nodes, claims; } sh_package_link_history_mark;
sh_package_link_history_mark sh_package_link_history_mark_now(const sh_package_link_history *history);
int sh_package_link_history_rollback(sh_package_link_history *history, sh_package_link_history_mark mark);
/* Export (R-344 stage B): node index's binding fingerprint and record length, its record bytes, and the
 * claims (private native name and ID per node) in the order they were issued. */
int sh_package_link_history_node(const sh_package_link_history *history, size_t index,
    unsigned char identity[32], uint64_t *length);
int sh_package_link_history_record(const sh_package_link_history *history, size_t index,
    int (*write)(void *, const void *, size_t), void *sink, char *error, size_t capacity);
size_t sh_package_link_history_claims(const sh_package_link_history *history);
int sh_package_link_history_claim(const sh_package_link_history *history, size_t index,
    const char **family, const char **name, uint32_t *id, size_t *node);
/* Import into an empty history (normally a fresh one, swapped in by the caller only once everything else of
 * a warm start was accepted): nodes with their records concatenated in node order, then claims. Distinct
 * fingerprints, claims naming nodes, names and IDs distinct per family; all or nothing (a failed import leaves
 * the history empty). The records are copied; the history owns its own. */
typedef struct sh_package_link_history_row { const char *family, *name; uint32_t id; size_t node; } sh_package_link_history_row;
int sh_package_link_history_import(sh_package_link_history *history, const unsigned char (*identities)[32],
    const uint64_t *lengths, size_t count, const unsigned char *records, uint64_t record_length,
    const sh_package_link_history_row *claims, size_t claim_count, char *error, size_t capacity);

/* Capture reuse across identity passes over one unit array. A unit's capture
 * (file identities, bindings, witnesses, implicit bindings) and the exact-byte
 * equality proofs between captured units are reused while the unit stays
 * valid. The owner invalidates a unit before any pass in which its files,
 * compilation, semantics, witnesses or binding behavior may differ; an
 * invalidated unit is captured afresh on every later pass. Never share one
 * reuse object between different unit arrays or adapters with different
 * resolve/bind behavior. */
typedef struct sh_package_link_reuse sh_package_link_reuse;
sh_package_link_reuse *sh_package_link_reuse_open(size_t count);
/* Grow for appended units without discarding valid prefix captures. Existing
 * unit indices and their source/binding contracts must remain unchanged;
 * invalidate any changed or relocated owned sources first. Never shrinks.
 * Failure leaves all captures and equality proofs intact. */
int sh_package_link_reuse_reserve(sh_package_link_reuse *reuse, size_t count);
void sh_package_link_reuse_invalidate(sh_package_link_reuse *reuse, size_t unit);
void sh_package_link_reuse_close(sh_package_link_reuse *reuse);
/* Diagnostic totals of the identity passes that used this cache, as one line:
 * units, units restored from it, and collect, partition and clear time per pass. */
const char *sh_package_link_reuse_counters(const sh_package_link_reuse *reuse, char *out, size_t capacity);
/* Diagnostic durations of the final link's steps that used this cache, or "". */
const char *sh_package_link_reuse_link_steps(const sh_package_link_reuse *reuse, char *out, size_t capacity);
/* Diagnostic meters of each identity pass on one line (or ""): units captured afresh and,
 * for units the cache never keeps (sticky-invalid), how many recaptures reproduced their
 * previous capture; and what the conditional-binding certification walked and removed.
 * Truncates to capacity; never changes a result. */
const char *sh_package_link_reuse_detail(const sh_package_link_reuse *reuse, char *out, size_t capacity);
/* Capture templates kept across compiles (R-348). A template's slot keys and slot order are a function of its file's
 * engine path and its sites' slots alone, so what one compile made of a file holds for a later compile's file with the
 * same path and equal slots, whatever its content and targets. encode writes this cache's templates as portable bytes
 * (malloc-owned; 0 on allocation failure): per file its path's hash and per site its slot, its occurrence and its
 * order position, never a key or a target. A later cache is offered such bytes before its first identity pass (offer
 * takes them in every case; 1 indexed, 0 malformed and dropped, -1 memory); that pass's first capture of a stable file
 * with no template yet takes the first offered one under its path whose slots equal the file's sites' slots one by
 * one: its keys are built from the file's own path into this cache's texts, as a fresh capture makes them, with the
 * recorded occurrences and order. Content digests and targets are the file's own, as for any template. The offer is
 * released when that pass has captured. */
int sh_package_link_reuse_templates_encode(const sh_package_link_reuse *reuse, unsigned char **bytes, size_t *length);
int sh_package_link_reuse_offer_templates(sh_package_link_reuse *reuse, unsigned char *bytes, size_t length);
/* Offered templates, those taken and those refused (a recorded file under the same path whose slots differed). */
void sh_package_link_reuse_offer_counts(const sh_package_link_reuse *reuse, size_t *offered, size_t *taken,
    size_t *refused);
#ifdef SH_PACKAGE_LINKER_TESTING
/* Tests: restore by copying a capture's arrays instead of borrowing them
 * (copy 1), and fail the n-th next copy-on-write of borrowed bindings
 * (fail_unshare 0 never). Counts are restores that borrowed and the binding
 * arrays copied before conditional bindings were removed. */
void sh_package_link_reuse_test_mode(sh_package_link_reuse *reuse, int copy, size_t fail_unshare);
void sh_package_link_reuse_test_counts(const sh_package_link_reuse *reuse,
    unsigned long long *borrowed, unsigned long long *unshared);
/* Tests: one identity pass's meters (0 when the pass does not exist), and failing the
 * allocation of the recapture meter's table (nothing else may change). */
typedef struct sh_package_link_pass_meters {
    size_t fresh, again_first, again_same, again_changed;
    size_t cond_units, cond_edges, cond_removed, cond_nonlocal, cond_certified, cond_walk_edges, cond_compares;
    unsigned long long cond_digest;
    size_t ordered; /* nodes the partition read through their capture's slot order (R-343) */
    /* Capture templates (R-348): files that took one, templates made, edges and single-file digests taken, and
     * templates made from an offered record. */
    size_t template_files, template_built, template_edges, template_digests, template_restored;
} sh_package_link_pass_meters;
int sh_package_link_reuse_test_meters(const sh_package_link_reuse *reuse, size_t pass, sh_package_link_pass_meters *out);
void sh_package_link_reuse_test_fail_meters(sh_package_link_reuse *reuse, int fail);
/* Tests: compare binding slots one by one instead of searching a sorted index,
 * as when the index cannot be allocated (only cond_compares may change). */
void sh_package_link_reuse_test_fail_index(sh_package_link_reuse *reuse, int fail);
/* Tests: the binding slots this cache keeps for a captured unit, in capture
 * order (up to capacity); returns how many it keeps, 0 if none. */
size_t sh_package_link_reuse_test_slots(const sh_package_link_reuse *reuse, size_t unit,
    const char **slots, size_t capacity);
#endif
#if defined(SH_PACKAGE_LINKER_TESTING) || defined(SH_PACKAGE_LIBRARY_TESTING) || \
    defined(SH_PACKAGE_SOURCE_LINK_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
/* Tests and measurement (R-348): 0 makes every first capture of a stable file afresh, as without capture templates
 * (the default, 1, takes them). */
void sh_package_link_test_templates(int enabled);
/* Tests (R-348), controls that must make an offered template differ from a fresh one: 1 takes one without comparing
 * its slots, 2 drops its occurrences, 3 drops its order (0 none). */
void sh_package_link_test_offer_mutation(int mutation);
#endif

typedef struct sh_package_link_implicit_binding {
    size_t owner, target; /* Equivalence-class indices in this output. */
    char *slot;          /* Owned original unit-local role. */
} sh_package_link_implicit_binding;

/* One derived publication: target's bytes served at path under name, for
 * owner's slot (sh_package_link_adapter.derive). Classes are output indices. */
typedef struct sh_package_link_derived {
    size_t owner, target;
    char *slot, *path, *name; /* owned */
} sh_package_link_derived;

typedef struct sh_package_link_output {
    sh_package_compilation *compiled;
    sh_package_link_classes classes;
    char **names; /* class -> native identity; unit -> classes.nodes[unit] */
    /* One row per deduplicated owner/slot. Prepare native consumer bindings
     * from these captured classes and the input units before releasing either.
     * These are not arbitrary string rewrites or additional resource objects. */
    sh_package_link_implicit_binding *implicit;
    size_t implicit_count;
    sh_package_link_derived *derived;
    size_t derived_count;
    /* class -> 1 when the class is carried (sh_package_link_unit.carried) and
     * changed: it is served only at its derived paths, not under names[class]. */
    unsigned char *carried;
    /* class -> 1 when its binding is certified original: its own bytes and
     * everything it binds are the installed ones (a unit's original_binding
     * covers only its own bytes). */
    unsigned char *originals;
    /* class -> identity provenance (R-397): every package whose content entered the class's identity or bytes:
     * its members' files' owners and provenance, closed over the edges its identity hashed. NULL unless some
     * unit carries provenance. */
    sh_package_owners *provenance;
    /* compiled resource -> the union of the provenance of every class published there (R-397); NULL with provenance. */
    sh_package_owners *row_provenance;
    const char **row_provenance_paths; /* each entry's engine path (borrowed from compiled) */
    size_t row_provenance_count; /* the compiled resource count when it was recorded */
} sh_package_link_output;

/* Collect exact source identities and scoped edges, dedupe whole bindings,
 * assign names, replay verified bindings, then publish one provider. Two
 * identical source files with different dependency bindings remain variants.
 * Matching source fingerprints are verified by exact streamed comparison
 * before suppliers share a class. Native implicit semantics remain the reader
 * adapter's certified input; a semantics fingerprint alone is not that proof.
 * Equivalent units emit one copy of every physical file and retain all source
 * owners. A changed/unavailable source, unresolved edge, unstable adapter or
 * alias collision aborts the entire candidate. No partial provider escapes.
 * Output starts zeroed; repeated calls replace it, including on failure. */
int sh_package_link(const sh_package_sources *sources,
    const sh_package_link_unit *units, size_t count,
    const sh_package_link_adapter *adapter, const sh_package_policy *policy,
    char *(*canonical_path)(const char *), sh_package_link_output *out,
    char *error, size_t capacity);
void sh_package_link_output_free(sh_package_link_output *out);

/* Freeze exact source/dependency equivalence before composing shared native
 * collections. Uses the same inspection and exact supplier comparison as link,
 * without assigning names, consulting native occupancy or projecting files.
 * Only resolve/bind are required. Classes belong to these retained inputs;
 * changing a unit or a dependency requires a new identification pass.
 * Initialize output to zero. Failure clears a previous result. */
int sh_package_link_identify_units(const sh_package_link_unit *units, size_t count,
    const sh_package_link_adapter *adapter, sh_package_link_classes *out,
    char *error, size_t capacity);

/* Capture only bindings referenced by a map's typed resource walk. roots uses
 * input unit indices; all equivalent suppliers of a selected class are retained
 * in one saved binding. unit_contexts maps each unit to an authored context, or
 * SIZE_MAX for an admitted zero-file native identity. Unused library contexts
 * never enter the result. This neither reparses map text nor decides delivery
 * availability. Inputs must be the same snapshot that produced linked.
 * Output starts zeroed, must differ from contexts, and clears on failure. */
int sh_package_link_provenance(const sh_package_link_output *linked,
    const sh_package_link_unit *units, size_t count,
    const sh_package_provenance *contexts, const size_t *unit_contexts,
    const size_t *roots, size_t root_count, sh_package_provenance *out,
    char *error, size_t capacity);

#endif
