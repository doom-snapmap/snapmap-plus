/* Native cached-resource refresh inside the existing provider transaction. */
#ifndef SH_RESOURCE_RESIDENT_H
#define SH_RESOURCE_RESIDENT_H
#include "resource_default_reads.h"
#include "resource_instances.h"
#include "package_compiler.h"
#include "resource_graph.h"
#include "signatures.h"

typedef struct sh_resource_resident sh_resource_resident;
int sh_resource_resident_bind(const sig_result *results, size_t count, const uint8_t *base);
/* Main thread, admitted native map/browser boundary. Captures identities before
 * parsing changes the graph; holds native renderer and process-heap scopes.
 * Recovery includes identities retained from the failed activation. Initial
 * publication runs before startup lifetime promotion, with no map loaded. */
sh_resource_resident *sh_resource_resident_begin(const sh_package_changes *changes,
    int restoring, int initial, char *error, size_t capacity);
/* Verify a previously admitted scope still owns its main thread, renderer and
 * heap. This permits recovery under held scopes without admitting new work. */
int sh_resource_resident_owned(const sh_resource_resident *pass);
/* Captured changed resources and their affected native consumers, including
 * identities retained for recovery. Unchanged loaded declarations stay intact. */
int sh_resource_resident_selected(const sh_resource_resident *pass, const void *resource);
/* As selected; out receives its type:name, why the pass selected it and its
 * captured state and lifetime, for ownership diagnostics. */
int sh_resource_resident_describe(const sh_resource_resident *pass, const void *resource, char *out, size_t capacity);
/* Additive editor admission requires zero selected existing objects before
 * entering a declaration parser or changing any native source registration. */
size_t sh_resource_resident_selected_count(const sh_resource_resident *pass);
/* As selected_count, only the objects of this type (case-insensitive); SIZE_MAX without a pass. */
size_t sh_resource_resident_selected_type(const sh_resource_resident *pass, const char *type);
/* The selected objects an additive admission may not touch: every one except
 * a revived shell, a defaulted object this provider retired, with no native
 * record, that only an added change names, and a refreshed shell, such an
 * object that is a recorded consumer of the addition (its default asked for an
 * added identity), whose default is rebuilt. first receives the first blocking
 * type:name and the reason it blocks. */
size_t sh_resource_resident_blocking_count(const sh_resource_resident *pass, char *first, size_t capacity);
/* Remove revived and refreshed shells from a sorted pre-admission ownership
 * list, so the addition owns them as fresh objects and recovery may retire
 * them again. A refreshed shell keeps its old default until
 * sh_resource_resident_defaults rebuilds it, after the addition's declarations
 * exist. Call before reconstruct. Returns the new count; logs each identity. */
size_t sh_resource_resident_release_revived(const sh_resource_resident *pass, void **existing, size_t count);
/* The recorded consumers of an additive pass's changed identities when it
 * began, before any preparation: moved to out (sorted) for its admission. */
int sh_resource_resident_take_prior(sh_resource_resident *pass, sh_resource_graph_impact *out);
/* Recovery: this pass selected resource only as a permanent recorded consumer,
 * and it was no consumer when the failed admission began (prior). Its edge was
 * recorded by that admission's preparation, so the admission owns restoring
 * it: recovery rebuilds it from the restored provider. Every other existing
 * selection, including inheritance and a consumer that predates the admission,
 * still refuses. */
int sh_resource_resident_acquired(const sh_resource_resident *pass, const void *resource,
    const sh_resource_graph_impact *prior);
/* Whether this pass revives exactly this object as this type:name: the same
 * provenance as the blocking rule, matched by address and identity. */
int sh_resource_resident_revives(const sh_resource_resident *pass, const void *resource,
    const char *type, const char *name);
/* A declaration pass owns these objects' reconstruction and pending marks.
 * Register exclusions before reconstruct. No object is rebuilt twice. */
void sh_resource_resident_external(sh_resource_resident *pass, void *resource);
int sh_resource_resident_reconstruct(sh_resource_resident *pass, char *error, size_t capacity);
/* Call after every declaration-owned pending mark exists, before its first
 * lookup. Drain also calls this for providers without declarations. Returns 0
 * only when called out of order. A retirement default that raised an engine
 * error, or that cannot be built in this process, does not stop the remaining
 * defaults or the pass's loads: it is recorded, and this pass's drain and end
 * fail with it. */
int sh_resource_resident_defaults(sh_resource_resident *pass, char *error, size_t capacity);
/* Engine default construction (discrete_default.c): enter and leave bracket this
 * module's defaults and loads on the calling execution stack; ready says whether a
 * family's default can be built in this process. A retirement it cannot build is
 * refused at begin; a scope that cannot be opened, or that does not end in order,
 * fails the pass. */
struct sh_discrete_scope;
void sh_resource_resident_set_default_construction(int (*enter)(struct sh_discrete_scope *scope),
    int (*leave)(struct sh_discrete_scope *scope), int (*ready)(const char *family));
/* Whether the object's engine default made every registered-resource read where the graph records
 * it, judged by the native default routine the object runs, not by its family (R-352). Asked inside a
 * retirement default's dependency frame (which also records the names it asked for and did not find),
 * after the default returned in a scope that ended in order; it may record there what the default
 * copied through a holder no hook sees. The frame completes only when it returns nonzero. Unset: no
 * retirement default's frame completes. */
void sh_resource_resident_set_default_observation(int (*observed)(void *resource));
/* Per-invocation probe before native destruction, consumed once afterward.
 * Resident owns separate stack input/receipt for every nested invocation.
 * prepare failure/exception never suppresses the native callback. finish runs
 * once while its graph frame is current, including native failure; in_order
 * also requires successful prepare and the default construction scope fence.
 * Set both callbacks or neither. With no pair, the old post observer is used. */
typedef void (*sh_resource_default_probe_prepare)(sh_resource_default_read_receipt *receipt,
    sh_resource_default_read_input *input, void *resource, void *actual_default);
typedef int (*sh_resource_default_probe_finish)(sh_resource_default_read_receipt *receipt,
    const sh_resource_default_read_input *input, int native_returned, int in_order);
void sh_resource_resident_set_default_probe(sh_resource_default_probe_prepare prepare,
    sh_resource_default_probe_finish finish);
/* Optional scoped invocation adapter. It invokes this actual callback once
 * or performs an explicitly qualified native correction in its place.
 * Zero is native construction failure; exceptions use the same cleanup path. */
typedef int (*sh_resource_default_invoke)(void *resource, void *actual_callback);
void sh_resource_resident_set_default_invoke(sh_resource_default_invoke invoke);
void sh_resource_resident_set_default_origins(
    int (*begin)(sh_resource_default_origin_scope *,void *),
    int (*finish)(sh_resource_default_origin_scope *,void *,int returned,int ordered,int complete));
void sh_resource_resident_set_instance_refresh(
    int (*refresh)(void *const *resources,size_t count,char *error,size_t capacity));
/* Admission runs under the held heap/render scopes, before reconstruction,
 * deferred defaults and subsequent reload/rebind boundaries. */
void sh_resource_resident_set_instance_admit(
    int (*admit)(void *const *resources,size_t count,char *error,size_t capacity));
/* What the last admission found at native holders whose actual inputs it could not
 * attribute: the holder (its kind and vtable RVA), the field (slot offset into the
 * holder, or SIZE_MAX for a typed path) and the input. A tolerated item's holder is
 * rebuilt from its inputs after the mutation, so the unattributed field cannot
 * outlive it; an isolated item's holder cannot be rebuilt, so the pass must leave
 * that input unmutated. The items stay valid until the next admission. */
typedef struct sh_resource_admit_report {
    void *holder; uintptr_t holder_vtable_rva; const char *holder_kind;
    uintptr_t slot; void *input; int unresolved_path; int tolerated;
} sh_resource_admit_report;
void sh_resource_resident_set_instance_report(
    size_t (*report)(const sh_resource_admit_report **items));
/* Resources this pass isolated: held by a native holder through an unattributed
 * field it cannot rebuild, so left as they are (not reconstructed, defaulted or
 * reloaded), each named in the log when it was isolated. */
size_t sh_resource_resident_isolated_count(const sh_resource_resident *pass);
int sh_resource_resident_isolated(const sh_resource_resident *pass, const void *resource);
int sh_resource_resident_isolated_identity(const sh_resource_resident *pass, const char *type, const char *name);
/* Capture copied native state before begin returns to a caller that may
 * reconstruct declarations. All callbacks run under the owning heap/render
 * exclusion. previous is a borrowed receipt from the failed pass: prepare
 * must preserve its original snapshots, and return an independent owning
 * reference in out (also when out equals previous). A failed prepare disposes
 * only its output; the previous receipt remains available for recovery.
 * finish runs after native loads/rebind, before ordinary instance refresh.
 * It may be retried during recovery and must retain the original snapshot
 * until dispose, including after a successful finish followed by pass failure.
 * dispose only releases receipt ownership; it must not call native objects.
 * Set all three callbacks or none. A callback change cannot consume an old
 * receipt made by a different implementation. */
typedef int (*sh_resource_instance_transfer_prepare)(void *const *resources,size_t count,
    const void *previous,void **out,char *error,size_t capacity);
typedef int (*sh_resource_instance_transfer_finish)(void *receipt,void *const *resources,
    size_t count,char *error,size_t capacity);
typedef void (*sh_resource_instance_transfer_dispose)(void *receipt);
void sh_resource_resident_set_instance_transfer(sh_resource_instance_transfer_prepare prepare,
    sh_resource_instance_transfer_finish finish,sh_resource_instance_transfer_dispose dispose);
/* Reload rebuilt resources whose family names no other engine resource, such
 * as skeletons and images, before any declaration is re-parsed. An object the
 * pass did not rebuild, a mesh for one, holds its skeleton directly and never
 * looks it up again. Call after defaults; drain still verifies every object. */
int sh_resource_resident_drain_leaves(sh_resource_resident *pass,
    int (*leaf)(const char *family), char *error, size_t capacity);
/* Reloads pending resources, then rebinds material virtual-texture parms inside
 * the heap-0 scope before the consumer update. Idempotent once it succeeds. */
int sh_resource_resident_drain(sh_resource_resident *pass, char *error, size_t capacity);
/* Active-editor additions/recovery use the sorted pre-admission native pointer
 * snapshot. No selected object may belong to that snapshot. Finalize only fresh
 * material/world-text caches; never run the whole-material rebind. Caller must
 * make this same exclusion check before reconstructing on recovery. */
int sh_resource_resident_drain_additions(sh_resource_resident *pass,
    void *const *existing, size_t existing_count, char *error, size_t capacity);
/* How many identities recovery left at the engine default because the restored
 * provider supplies no source for them. Reported, never hidden. */
size_t sh_resource_resident_recovered_defaults(const sh_resource_resident *pass);
/* Native load/lookup observers preserve existing and newly loaded dependency
 * lifetimes only while the admitted main-thread refresh scope owns them. */
void sh_resource_resident_touch(void *resource);
/* Always call, including native exceptions. Disarms leftover owned marks,
 * restores scopes, retains failed identities for provider recovery. */
int sh_resource_resident_end(sh_resource_resident *pass, int succeeded,
    char *error, size_t capacity);
#endif
