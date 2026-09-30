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
    /* Process-lifetime exact binding proofs and native alias reservations.
     * The caller serializes access. Successful links reserve identities even
     * if later activation fails: native defaulted objects may still exist. */
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
} sh_package_link_adapter;

/* Owns small identity metadata and sealed, streamed proofs in an admitted
 * private cache directory. Never pins editable authored files. Repeated equal
 * compilations add neither proofs nor reservations. Close only after all native
 * identities in this registry cease to exist (normally process teardown). */
typedef struct sh_package_link_history sh_package_link_history;
sh_package_link_history *sh_package_link_history_open(const char *directory);
void sh_package_link_history_close(sh_package_link_history *history);
size_t sh_package_link_history_count(const sh_package_link_history *history);

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
 * units, units restored from it, and collect and partition time per pass. */
const char *sh_package_link_reuse_counters(const sh_package_link_reuse *reuse, char *out, size_t capacity);
/* Diagnostic durations of the final link's steps that used this cache, or "". */
const char *sh_package_link_reuse_link_steps(const sh_package_link_reuse *reuse, char *out, size_t capacity);

typedef struct sh_package_link_implicit_binding {
    size_t owner, target; /* Equivalence-class indices in this output. */
    char *slot;          /* Owned original unit-local role. */
} sh_package_link_implicit_binding;

typedef struct sh_package_link_output {
    sh_package_compilation *compiled;
    sh_package_link_classes classes;
    char **names; /* class -> native identity; unit -> classes.nodes[unit] */
    /* One row per deduplicated owner/slot. Prepare native consumer bindings
     * from these captured classes and the input units before releasing either.
     * These are not arbitrary string rewrites or additional resource objects. */
    sh_package_link_implicit_binding *implicit;
    size_t implicit_count;
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
